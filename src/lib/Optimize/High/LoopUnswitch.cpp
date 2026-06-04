#include "Optimize/High/LoopUnswitch.h"
#include "Optimize/High/HighDCE.h"
#include "Optimize/Utils/PatternMatch.h"
#include "IR/IRRewriter.h"
#include "IR/IRBuilder.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace sysy;

// Avoid turning a cheap branch into too much duplicated loop body.
static constexpr int MaxIVSplitCost = 48;

bool LoopUnswitch::isLoadFrom(Value* v, Value* addr) {
    auto* load = dyn_cast<LoadInst>(v);
    return load && load->getOperand(0) == addr;
}

bool LoopUnswitch::getConstInt(Value* v, int& value) {
    auto* c = dyn_cast<ConstantInt>(v);
    if (!c)
        return false;
    value = c->getValue();
    return true;
}

LoopUnswitch::IVExpr LoopUnswitch::matchIVExpr(Value* v, Value* ivAddr) {
    // Match load(iv) + const so guards like i + 1 < n can be split.
    if (isLoadFrom(v, ivAddr))
        return {true, 0};

    auto* bin = dyn_cast<BinaryInst>(v);
    if (!bin)
        return {};

    int c = 0;
    if (bin->getOpID() == Instruction::Add) {
        if (getConstInt(bin->getOperand(1), c)) {
            auto base = matchIVExpr(bin->getOperand(0), ivAddr);
            if (base.matched)
                return {true, base.offset + c};
        }
        if (getConstInt(bin->getOperand(0), c)) {
            auto base = matchIVExpr(bin->getOperand(1), ivAddr);
            if (base.matched)
                return {true, base.offset + c};
        }
    }

    if (bin->getOpID() == Instruction::Sub &&
        getConstInt(bin->getOperand(1), c)) {
        auto base = matchIVExpr(bin->getOperand(0), ivAddr);
        if (base.matched)
            return {true, base.offset - c};
    }

    return {};
}

LoopUnswitch::ModExpr LoopUnswitch::matchIVModExpr(Value* v, Value* ivAddr) {
    auto* bin = dyn_cast<BinaryInst>(v);
    if (!bin || bin->getOpID() != Instruction::Mod)
        return {};

    int divisor = 0;
    if (!getConstInt(bin->getOperand(1), divisor) || divisor == 0)
        return {};

    auto iv = matchIVExpr(bin->getOperand(0), ivAddr);
    if (!iv.matched)
        return {};

    if (divisor < 0)
        divisor = -divisor;
    return {true, iv, divisor};
}

void LoopUnswitch::collectLoadAddrs(Value* v, std::set<Value*>& addrs, std::set<Value*>& visiting) {
    if (!v || visiting.count(v)) return;
    visiting.insert(v);
    auto* inst = dyn_cast<Instruction>(v);
    if (!inst) return;
    if (auto* ld = dyn_cast<LoadInst>(inst)) {
        addrs.insert(ld->getOperand(0));
        collectLoadAddrs(ld->getOperand(0), addrs, visiting);
        return;
    }
    if (!inst->isPureCloneable()) return;
    for (int i = 0; i < inst->getNumOperands(); ++i)
        collectLoadAddrs(inst->getOperand(i), addrs, visiting);
}

Value* LoopUnswitch::stripGEP(Value* v) {
    // Compare memory objects, not individual GEP offsets.
    while (auto* gep = dyn_cast<GetElementPtrInst>(v))
        v = gep->getOperand(0);
    return v;
}

bool LoopUnswitch::instStoresToBase(Instruction* inst, Value* base) {
    if (!inst || !base)
        return true;

    if (auto* st = dyn_cast<StoreInst>(inst))
        return stripGEP(st->getOperand(1)) == base;
    if (auto* call = dyn_cast<CallInst>(inst)) {
        for (int i = 0; i < call->getNumOperands(); ++i)
            if (stripGEP(call->getOperand(i)) == base)
                return true;
        return false;
    }
    if (auto* ii = dyn_cast<IfInst>(inst))
        return regionStoresToBase(ii->getThenRegion(), base) ||
            (ii->getElseRegion() && regionStoresToBase(ii->getElseRegion(), base));
    if (auto* fi = dyn_cast<ForInst>(inst))
        return regionStoresToBase(fi->getBodyRegion(), base);
    if (auto* wi = dyn_cast<WhileInst>(inst))
        return regionStoresToBase(wi->getCondRegion(), base) ||
            regionStoresToBase(wi->getBodyRegion(), base);

    return false;
}

bool LoopUnswitch::regionStoresToBase(Region* region, Value* base) {
    if (!region)
        return false;
    for (auto* bb : region->getBlocks())
        for (auto* inst : bb->getInstructions())
            if (instStoresToBase(inst, base))
                return true;
    return false;
}

bool LoopUnswitch::loadsStableInLoop(
        Region* region, Value* ivAddr,
        const std::set<Value*>& loadAddrs,
        const std::set<Value*>& loopDefs) {
    for (auto* addr : loadAddrs) {
        if (addr == ivAddr)
            continue;

        // A load is safe to hoist/split only if both its address and memory stay stable.
        std::set<Value*> visiting;
        if (valueDependsOnLoop(addr, loopDefs, visiting))
            return false;

        Value* base = stripGEP(addr);
        if (!isa<AllocaInst>(base))
            return false;
        if (regionStoresToBase(region, base))
            return false;
    }
    return true;
}

int LoopUnswitch::instCost(Instruction* inst) {
    if (!inst)
        return MaxIVSplitCost + 1;
    if (isa<FlowInst>(inst) || isa<ContinueInst>(inst))
        return 0;
    if (auto* ii = dyn_cast<IfInst>(inst))
        return 1 + regionCost(ii->getThenRegion()) +
            (ii->getElseRegion() ? regionCost(ii->getElseRegion()) : 0);
    if (auto* fi = dyn_cast<ForInst>(inst))
        return MaxIVSplitCost + 1;
    if (auto* wi = dyn_cast<WhileInst>(inst))
        return MaxIVSplitCost + 1;
    return 1;
}

int LoopUnswitch::regionCost(Region* region) {
    if (!region)
        return 0;
    int cost = 0;
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            cost += instCost(inst);
            if (cost > MaxIVSplitCost)
                return cost;
        }
    }
    return cost;
}

bool LoopUnswitch::splitCostIsSmall(
        ForInst* fi, IfInst* guard,
        const std::vector<Instruction*>& prefix,
        const std::vector<Instruction*>& suffix) {
    // Unswitching duplicates the selected path; keep it profitable for hot loops.
    int cost = static_cast<int>(prefix.size() + suffix.size());
    if (guard)
        cost += regionCost(guard->getThenRegion()) +
            (guard->getElseRegion() ? regionCost(guard->getElseRegion()) : 0);

    return cost <= MaxIVSplitCost;
}

bool LoopUnswitch::selectedRegionIsSimple(IfInst* guard, bool takeThen) {
    Region* region = takeThen ? guard->getThenRegion() : guard->getElseRegion();
    return !region || region->getBlocks().size() == 1;
}

void LoopUnswitch::collectDefs(Region* region, std::set<Value*>& defs) {
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            defs.insert(inst);
            if (auto* wi = dyn_cast<WhileInst>(inst)) {
                for (auto* rv : wi->getResults()) defs.insert(rv);
                collectDefs(wi->getCondRegion(), defs);
                collectDefs(wi->getBodyRegion(), defs);
            } else if (auto* fi = dyn_cast<ForInst>(inst)) {
                collectDefs(fi->getBodyRegion(), defs);
            } else if (auto* ii = dyn_cast<IfInst>(inst)) {
                for (auto* rv : ii->getResults()) defs.insert(rv);
                collectDefs(ii->getThenRegion(), defs);
                if (ii->getElseRegion()) collectDefs(ii->getElseRegion(), defs);
            }
        }
    }
}

bool LoopUnswitch::valueDependsOnLoop(Value* v, const std::set<Value*>& loopDefs, std::set<Value*>& visiting) {
    if (!v || !loopDefs.count(v)) return false;
    auto* inst = dyn_cast<Instruction>(v);
    if (!inst) return true;
    if (!inst->isPureCloneable()) return true;
    if (visiting.count(v)) return false;
    visiting.insert(v);
    for (int i = 0; i < inst->getNumOperands(); ++i)
        if (valueDependsOnLoop(inst->getOperand(i), loopDefs, visiting)) return true;
    visiting.erase(v);
    return false;
}

bool LoopUnswitch::isInvariantValue(Value* v, const std::set<Value*>& loopDefs) {
    std::set<Value*> visiting;
    return !valueDependsOnLoop(v, loopDefs, visiting);
}

bool LoopUnswitch::absDivisor(int value, int& out) {
    if (value == INT_MIN) return false;
    out = std::abs(value);
    return out >= 2;
}

Value* LoopUnswitch::addConst(Value* v, int delta, BasicBlock* bb, InstIter pos, const std::string& name) {
    if (delta == 0) return v;
    if (auto* c = dyn_cast<ConstantInt>(v))
        return new ConstantInt(c->getValue() + delta);
    IRBuilder builder;
    builder.SetInsertPoint(bb, pos);
    auto* add = builder.InsertNew<BinaryInst>(Instruction::Add, v, new ConstantInt(delta));
    add->setName(name);
    return add;
}

Value* LoopUnswitch::matBefore(Value* v, BasicBlock* src, BasicBlock* dst, InstIter pos, std::map<Value*, Value*>& vmap) {
    if (!v) return nullptr;
    auto it = vmap.find(v);
    if (it != vmap.end()) return it->second;
    auto* inst = dyn_cast<Instruction>(v);
    if (!inst || inst->getParent() != src || !inst->isPureCloneable()) return v;
    for (int i = 0; i < inst->getNumOperands(); ++i) {
        Value* op = inst->getOperand(i);
        Value* mapped = matBefore(op, src, dst, pos, vmap);
        if (mapped && mapped != op) vmap[op] = mapped;
    }
    auto* cloned = inst->clone(vmap);
    if (!cloned) return v;
    cloned->setName(inst->getName());
    IRBuilder builder;
    builder.SetInsertPoint(dst, pos);
    builder.Insert(cloned);
    if (!cloned->getType()->isVoid()) vmap[v] = cloned;
    return cloned;
}

Value* LoopUnswitch::alignStart(
        Value* start, Value* split, int step, bool inc,
        BasicBlock* bb, InstIter pos,
        const std::string& tag) {
    int unit = inc ? step : -step;
    if (unit <= 0) return start;
    if (unit == 1)
        return IRRewriter::buildMinOrMax(start, split, /*wantMin=*/!inc, bb, pos);

    auto* sc = dyn_cast<ConstantInt>(start);
    auto* pc = dyn_cast<ConstantInt>(split);
    if (sc && pc) {
        int s = sc->getValue(), p = pc->getValue();
        if (inc) {
            if (p <= s) return start;
            int diff = p - s, q = (diff + unit - 1) / unit;
            return new ConstantInt(s + q * unit);
        }
        if (p >= s) return start;
        int diff = s - p, q = (diff + unit - 1) / unit;
        return new ConstantInt(s - q * unit);
    }

    IRBuilder builder;
    builder.SetInsertPoint(bb, pos);

    auto* cmp = builder.InsertNew<ICmpInst>(inc ? ICmpInst::SLE : ICmpInst::SGE, split, start);
    auto* sel = builder.InsertNew<IfInst>(cmp);
    sel->setName(tag);
    sel->addElseRegion();
    auto* result = sel->createResult(Type::getIntTy());
    result->setName(tag + ".v");

    auto* thenBB = builder.CreateBlock("align.t", sel->getThenRegion());
    builder.SetInsertPoint(thenBB);
    builder.InsertNew<FlowInst>(std::vector<Value*>{start});

    auto* elseBB = builder.CreateBlock("align.e", sel->getElseRegion());
    builder.SetInsertPoint(elseBB);

    auto* diff = builder.InsertNew<BinaryInst>(
            Instruction::Sub,
            inc ? split : start, inc ? start : split);
    auto* bias = builder.InsertNew<BinaryInst>(Instruction::Add, diff, new ConstantInt(unit - 1));
    auto* q = builder.InsertNew<BinaryInst>(Instruction::Div, bias, new ConstantInt(unit));
    auto* scaled = builder.InsertNew<BinaryInst>(Instruction::Mul, q, new ConstantInt(unit));
    auto* raw = builder.InsertNew<BinaryInst>(
            inc ? Instruction::Add : Instruction::Sub,
            start, scaled);
    auto* ov = builder.InsertNew<ICmpInst>(inc ? ICmpInst::SLT : ICmpInst::SGT, raw, start);

    auto* sat = builder.InsertNew<IfInst>(ov);
    sat->setName(tag + ".sat");
    sat->addElseRegion();
    auto* satVal = sat->createResult(Type::getIntTy());
    satVal->setName(tag + ".start");

    auto* satThen = builder.CreateBlock("sat.ov", sat->getThenRegion());
    builder.SetInsertPoint(satThen);
    builder.InsertNew<FlowInst>(std::vector<Value*>{
        new ConstantInt(inc ? std::numeric_limits<int>::max() : std::numeric_limits<int>::min())});

    auto* satElse = builder.CreateBlock("sat.ok", sat->getElseRegion());
    builder.SetInsertPoint(satElse);
    builder.InsertNew<FlowInst>(std::vector<Value*>{raw});

    builder.SetInsertPoint(elseBB);
    builder.InsertNew<FlowInst>(std::vector<Value*>{satVal});
    return result;
}

bool LoopUnswitch::runInvariant() {
    bool any = false;
    for (auto* f : M->getFunctions())
        any |= runInvariantFunc(f);
    return any;
}

bool LoopUnswitch::run() {
    bool anyChanged = false;
    bool changed;
    sideLoops_.clear();
    do {
        changed = false;
        changed |= runInvariant();
        changed |= runIV();
        changed |= runMod();
        if (changed)
            HighDCE(M).run();
        anyChanged |= changed;
    } while (changed);
    return anyChanged;
}

IfInst* LoopUnswitch::firstIfWithPrefix(BasicBlock* bodyBB, std::vector<Instruction*>& prefix) {
    if (!bodyBB) return nullptr;
    for (auto* inst : bodyBB->getInstructions()) {
        if (auto* ii = dyn_cast<IfInst>(inst))
            return ii;
        if (inst->isTerminatingFlow())
            return nullptr;
        prefix.push_back(inst);
    }
    return nullptr;
}

bool LoopUnswitch::prefixIsPure(const std::vector<Instruction*>& prefix) {
    for (auto* inst : prefix)
        if (!inst->isPureCloneable())
            return false;
    return true;
}

bool LoopUnswitch::valueUsesAny(Value* v, const std::set<Value*>& defs, std::set<Value*>& visiting) {
    if (!v) return false;
    if (defs.count(v)) return true;
    auto* inst = dyn_cast<Instruction>(v);
    if (!inst || visiting.count(v)) return false;
    visiting.insert(v);
    for (int i = 0; i < inst->getNumOperands(); ++i)
        if (valueUsesAny(inst->getOperand(i), defs, visiting))
            return true;
    return false;
}

bool LoopUnswitch::buildInvariantPrefix(
        Value* guardCond,
        const std::vector<Instruction*>& bodyPrefix,
        std::vector<Instruction*>& outerPrefix) {
    // Hoist only the prefix needed to materialize the loop-invariant guard.
    if (prefixIsPure(bodyPrefix)) {
        outerPrefix = bodyPrefix;
        return true;
    }

    std::set<Value*> prefixDefs(bodyPrefix.begin(), bodyPrefix.end());
    std::set<Value*> visiting;
    if (valueUsesAny(guardCond, prefixDefs, visiting))
        return false;

    outerPrefix.clear();
    return true;
}

bool LoopUnswitch::guardLoadsStable(WhileInst* wi, IfInst* guard, const std::vector<Instruction*>& prefix) {
    std::set<Value*> loadAddrs;
    std::set<Value*> visiting;

    collectLoadAddrs(guard->getOperand(0), loadAddrs, visiting);
    for (auto* inst : prefix)
        collectLoadAddrs(inst, loadAddrs, visiting);

    std::set<Value*> loopDefs;
    collectDefs(wi->getCondRegion(), loopDefs);
    collectDefs(wi->getBodyRegion(), loopDefs);
    for (auto* rv : wi->getResults())
        loopDefs.insert(rv);
    return loadsStableInLoop(wi->getCondRegion(), nullptr, loadAddrs, loopDefs) &&
        loadsStableInLoop(wi->getBodyRegion(), nullptr, loadAddrs, loopDefs);
}

bool LoopUnswitch::findInvariant(WhileInst* wi, InvariantInfo& ii) {
    // Detect: while (...) { if (loop-invariant-cond) A else B; }
    if (!wi)
        return false;

    std::vector<Instruction*> bodyPrefix;
    IfInst* guard = firstIfWithPrefix(wi->getBodyRegion()->getEntryBlock(), bodyPrefix);
    if (!guard)
        return false;
    if (!selectedRegionIsSimple(guard, true) ||
        !selectedRegionIsSimple(guard, false))
        return false;

    std::vector<Instruction*> outerPrefix;
    if (!buildInvariantPrefix(guard->getOperand(0), bodyPrefix, outerPrefix))
        return false;

    std::set<Value*> loopDefs;
    collectDefs(wi->getCondRegion(), loopDefs);
    collectDefs(wi->getBodyRegion(), loopDefs);
    for (auto* rv : wi->getResults())
        loopDefs.insert(rv);

    if (!isInvariantValue(guard->getOperand(0), loopDefs))
        return false;
    if (!guardLoadsStable(wi, guard, outerPrefix))
        return false;

    ii.guard = guard;
    ii.prefix = outerPrefix;
    return true;
}

bool LoopUnswitch::findInvariantFor(ForInst* fi, InvariantInfo& ii) {
    // Detect: for (...) { if (loop-invariant-cond) A else B; }
    if (!fi) return false;

    auto* bodyBB = fi->getBodyRegion()->getEntryBlock();
    std::vector<Instruction*> bodyPrefix;
    IfInst* guard = firstIfWithPrefix(bodyBB, bodyPrefix);
    if (!guard) return false;
    if (!selectedRegionIsSimple(guard, true) || !selectedRegionIsSimple(guard, false))
        return false;

    std::vector<Instruction*> outerPrefix;
    if (!buildInvariantPrefix(guard->getOperand(0), bodyPrefix, outerPrefix))
        return false;

    std::set<Value*> loopDefs;
    collectDefs(fi->getBodyRegion(), loopDefs);

    if (!isInvariantValue(guard->getOperand(0), loopDefs))
        return false;

    Value* ivAddr = fi->getIVAddr();

    std::set<Value*> condLoadAddrs;
    std::set<Value*> visiting;
    collectLoadAddrs(guard->getOperand(0), condLoadAddrs, visiting);
    if (condLoadAddrs.count(ivAddr))
        return false;

    std::set<Value*> prefixLoadAddrs;
    for (auto* inst : outerPrefix)
        collectLoadAddrs(inst, prefixLoadAddrs, visiting);
    condLoadAddrs.insert(prefixLoadAddrs.begin(), prefixLoadAddrs.end());
    if (!loadsStableInLoop(fi->getBodyRegion(), ivAddr, condLoadAddrs, loopDefs))
        return false;

    ii.guard = guard;
    ii.prefix = outerPrefix;
    return true;
}

bool LoopUnswitch::runInvariantFunc(Function* f) {
    if (!f || f->getBody()->getBlocks().empty())
        return false;
    return processInvariantRegion(f->getBody());
}

bool LoopUnswitch::processInvariantRegion(Region* region) {
    return IRRewriter::rewriteRegion(region, [&](Instruction* inst) {
        if (auto* wi = dyn_cast<WhileInst>(inst))
            return processInvariantWhile(wi);
        if (auto* fi = dyn_cast<ForInst>(inst))
            return processInvariantFor(fi);
        return false;
    });
}

bool LoopUnswitch::applyInvariant(
    Instruction* loop,
    const InvariantInfo& inv,
    const std::string& tag,
    const std::vector<ResultValue*>& sourceResults,
    const std::function<Instruction*(std::map<Value*, Value*>&)>& cloneLoop,
    const std::function<void(Instruction*, std::vector<Value*>&)>& collectFlow,
    const std::function<void(const std::vector<ResultValue*>&)>& replaceResults) {
    auto* parent = loop ? loop->getParent() : nullptr;
    if (!parent)
        return false;
    auto& insts = parent->getInstructions();
    auto pos = IRRewriter::findInst(parent, loop);
    if (pos == insts.end())
        return false;

    // Build: if (cond) cloned-loop-with-then else cloned-loop-with-else.
    std::map<Value*, Value*> condMap;
    std::vector<Instruction*> clonedPrefix;
    for (auto* inst : inv.prefix) {
        auto* cloned = inst->clone(condMap);
        if (!cloned) return false;
        cloned->setName(inst->getName() + ".inv");
        clonedPrefix.push_back(cloned);
        if (!cloned->getType()->isVoid())
            condMap[inst] = cloned;
    }

    Value* guardCond = inv.guard->getOperand(0);
    Value* remappedCond = condMap.count(guardCond) ? condMap[guardCond] : guardCond;
    auto* outerIf = IRRewriter::makeIf(remappedCond, tag + "_if", true);
    std::vector<ResultValue*> outerResults;
    for (auto* rv : sourceResults) {
        auto* newRV = outerIf->createResult(rv->getType());
        newRV->setName(rv->getName());
        outerResults.push_back(newRV);
    }

    auto buildBranch = [&](
            bool takeThen,
            const std::string& bbName,
            Region* region,
            IfInst*& clonedGuard) -> bool {
        std::map<Value*, Value*> branchMap;
        Instruction* clonedLoop = cloneLoop(branchMap);
        if (!clonedLoop)
            return false;
        clonedGuard = dyn_cast<IfInst>(branchMap[inv.guard]);
        if (!clonedGuard)
            return false;

        auto* bb = IRRewriter::makeBlock(region, bbName);
        IRRewriter::appendInst(bb, clonedLoop);
        if (!outerResults.empty()) {
            std::vector<Value*> vals;
            collectFlow(clonedLoop, vals);
            IRRewriter::appendFlow(bb, vals);
        }
        return true;
    };

    IfInst* thenGuard = nullptr;
    IfInst* elseGuard = nullptr;
    if (!buildBranch(true, tag + "_then", outerIf->getThenRegion(), thenGuard) ||
        !buildBranch(false, tag + "_else", outerIf->getElseRegion(), elseGuard))
        return false;

    if (!IRRewriter::inlineSelectedBranch(thenGuard, true))
        return false;
    if (!IRRewriter::inlineSelectedBranch(elseGuard, false))
        return false;

    for (auto* cloned : clonedPrefix)
        IRRewriter::insertInst(parent, pos, cloned);
    IRRewriter::insertInst(parent, pos, outerIf);
    replaceResults(outerResults);
    IRRewriter::eraseOp(loop);
    return true;
}

bool LoopUnswitch::processInvariantWhile(WhileInst* wi) {
    InvariantInfo inv;
    if (!findInvariant(wi, inv))
        return false;

    std::vector<ResultValue*> sourceResults(wi->getResults().begin(), wi->getResults().end());
    return applyInvariant(
        wi, inv, "hu", sourceResults,
        [&](std::map<Value*, Value*>& vmap) -> Instruction* {
            return wi->clone(vmap);
        },
        [](Instruction* loop, std::vector<Value*>& vals) {
            auto* cloned = dyn_cast<WhileInst>(loop);
            if (!cloned)
                return;
            for (unsigned i = 0; i < cloned->getNumResults(); ++i)
                vals.push_back(cloned->getResult(i));
        },
        [&](const std::vector<ResultValue*>& outerResults) {
            for (unsigned i = 0; i < outerResults.size(); ++i)
                wi->getResult(i)->replaceAllUsesWith(outerResults[i]);
        });
}

bool LoopUnswitch::processInvariantFor(ForInst* fi) {
    InvariantInfo inv;
    if (!findInvariantFor(fi, inv))
        return false;

    return applyInvariant(
        fi, inv, "li", {},
        [&](std::map<Value*, Value*>& vmap) -> Instruction* {
            return fi->clone(vmap);
        },
        [](Instruction*, std::vector<Value*>&) {},
        [](const std::vector<ResultValue*>&) {});
}

ICmpInst::CmpOp LoopUnswitch::swapPred(ICmpInst::CmpOp pred) {
    switch (pred) {
        case ICmpInst::SGT: return ICmpInst::SLT;
        case ICmpInst::SGE: return ICmpInst::SLE;
        case ICmpInst::SLT: return ICmpInst::SGT;
        case ICmpInst::SLE: return ICmpInst::SGE;
        default: return pred;
    }
}

bool LoopUnswitch::loadsAreStableInFor(
        ForInst* fi, Value* ivAddr, IfInst* guard,
        const std::vector<Instruction*>& prefix) {
    std::set<Value*> loadAddrs;
    std::set<Value*> visiting;

    collectLoadAddrs(guard->getOperand(0), loadAddrs, visiting);
    for (auto* inst : prefix)
        collectLoadAddrs(inst, loadAddrs, visiting);

    std::set<Value*> loopDefs;
    collectDefs(fi->getBodyRegion(), loopDefs);
    return loadsStableInLoop(fi->getBodyRegion(), ivAddr, loadAddrs, loopDefs);
}

bool LoopUnswitch::guardTrueIsEarly(ICmpInst::CmpOp pred, bool increasing, bool& early) {
    if (increasing) {
        if (pred == ICmpInst::SLT || pred == ICmpInst::SLE) {
            early = true;
            return true;
        }
        if (pred == ICmpInst::SGT || pred == ICmpInst::SGE) {
            early = false;
            return true;
        }
    } else {
        if (pred == ICmpInst::SGT || pred == ICmpInst::SGE) {
            early = true;
            return true;
        }
        if (pred == ICmpInst::SLT || pred == ICmpInst::SLE) {
            early = false;
            return true;
        }
    }
    return false;
}

bool LoopUnswitch::regionIsPureContinue(Region* region) {
    if (!region || region->getBlocks().size() != 1)
        return false;

    auto* bb = region->getEntryBlock();
    if (!bb)
        return false;

    bool sawContinue = false;
    for (auto* inst : bb->getInstructions()) {
        if (isa<ContinueInst>(inst)) {
            sawContinue = true;
            continue;
        }
        if (sawContinue)
            return false;
        if (!inst->isPureCloneable())
            return false;
    }
    return sawContinue;
}

bool LoopUnswitch::instHasWork(Instruction* inst) {
    if (!inst)
        return false;

    if (isa<FlowInst>(inst) || isa<ContinueInst>(inst))
        return false;

    if (auto* ii = dyn_cast<IfInst>(inst))
        return regionHasWork(ii->getThenRegion()) ||
            (ii->getElseRegion() && regionHasWork(ii->getElseRegion()));
    if (auto* fi = dyn_cast<ForInst>(inst))
        return regionHasWork(fi->getBodyRegion());
    if (auto* wi = dyn_cast<WhileInst>(inst))
        return regionHasWork(wi->getCondRegion()) ||
            regionHasWork(wi->getBodyRegion());

    if (isa<StoreInst>(inst) || isa<CallInst>(inst) || isa<AllocaInst>(inst))
        return true;

    return !inst->isPureCloneable();
}

bool LoopUnswitch::regionHasWork(Region* region) {
    if (!region)
        return false;
    for (auto* bb : region->getBlocks())
        for (auto* inst : bb->getInstructions())
            if (instHasWork(inst))
                return true;
    return false;
}

bool LoopUnswitch::suffixHasWork(const std::vector<Instruction*>& suffix) {
    for (auto* inst : suffix)
        if (instHasWork(inst))
            return true;
    return false;
}

bool LoopUnswitch::pathHasWork(
        IfInst* guard,
        bool takeThen,
        const std::vector<Instruction*>& suffix) {
    if (!guard)
        return false;

    Region* branch = takeThen ? guard->getThenRegion() : guard->getElseRegion();
    if (regionHasWork(branch))
        return true;

    return pathFallsThrough(guard, takeThen) && suffixHasWork(suffix);
}

bool LoopUnswitch::hasOpaqueCall(Instruction* inst) {
    if (!inst)
        return false;

    if (auto* call = dyn_cast<CallInst>(inst)) {
        auto* callee = call->getFunction();
        return !callee || !callee->getBody() || callee->getBody()->getBlocks().empty();
    }

    if (auto* ii = dyn_cast<IfInst>(inst))
        return hasOpaqueCall(ii->getThenRegion()) ||
            (ii->getElseRegion() && hasOpaqueCall(ii->getElseRegion()));
    if (auto* fi = dyn_cast<ForInst>(inst))
        return hasOpaqueCall(fi->getBodyRegion());
    if (auto* wi = dyn_cast<WhileInst>(inst))
        return hasOpaqueCall(wi->getCondRegion()) ||
            hasOpaqueCall(wi->getBodyRegion());
    return false;
}

bool LoopUnswitch::hasOpaqueCall(Region* region) {
    if (!region)
        return false;
    for (auto* bb : region->getBlocks())
        for (auto* inst : bb->getInstructions())
            if (hasOpaqueCall(inst))
                return true;
    return false;
}

bool LoopUnswitch::pathFallsThrough(IfInst* guard, bool takeThen) {
    Region* region = takeThen ? guard->getThenRegion() : guard->getElseRegion();
    return !region || !regionIsPureContinue(region);
}

bool LoopUnswitch::matchIneq(
        Value* cond, Value* ivAddr,
        ICmpInst::CmpOp& pred,
        IVExpr& ivExpr,
        Value*& bound) {
    // Match guards of the form iv + c < bound or bound < iv + c.
    static Pattern patSlt("(slt x y)");
    static Pattern patSle("(sle x y)");
    static Pattern patSgt("(sgt x y)");
    static Pattern patSge("(sge x y)");
    struct Candidate { Pattern* pat; ICmpInst::CmpOp pred; };
    static Candidate candidates[] = {
        {&patSlt, ICmpInst::SLT},
        {&patSle, ICmpInst::SLE},
        {&patSgt, ICmpInst::SGT},
        {&patSge, ICmpInst::SGE},
    };

    for (auto& candidate : candidates) {
        if (!candidate.pat->match(cond))
            continue;

        Value* lhs = candidate.pat->extract("x");
        Value* rhs = candidate.pat->extract("y");
        IVExpr lhsExpr = matchIVExpr(lhs, ivAddr);
        IVExpr rhsExpr = matchIVExpr(rhs, ivAddr);

        if (lhsExpr.matched && !rhsExpr.matched) {
            pred = candidate.pred;
            ivExpr = lhsExpr;
            bound = rhs;
            return true;
        }
        if (rhsExpr.matched && !lhsExpr.matched) {
            pred = swapPred(candidate.pred);
            ivExpr = rhsExpr;
            bound = lhs;
            return true;
        }
    }
    return false;
}

bool LoopUnswitch::findForMatch(ForInst* fi, ForMatchInfo& mi) {
    // Detect a single if whose truth changes at one IV boundary.
    auto* stepConst = dyn_cast<ConstantInt>(fi->getStep());
    if (!stepConst)
        return false;

    int step = stepConst->getValue();
    bool increasing = step > 0;
    if (step == 0)
        return false;
    if (increasing) {
        if (fi->getPred() != ICmpInst::SLT)
            return false;
    } else {
        if (fi->getPred() != ICmpInst::SGT)
            return false;
    }

    auto* bodyBB = fi->getBodyRegion()->getEntryBlock();
    if (!bodyBB)
        return false;

    IfInst* guard = nullptr;
    std::vector<Instruction*> prefix;
    std::vector<Instruction*> suffix;
    bool seenGuard = false;
    for (auto* inst : bodyBB->getInstructions()) {
        if (auto* ii = dyn_cast<IfInst>(inst)) {
            if (seenGuard)
                return false;
            guard = ii;
            seenGuard = true;
            continue;
        }

        if (seenGuard) {
            if (inst->isTerminatingFlow())
                return false;
            suffix.push_back(inst);
            continue;
        }

        if (!inst->isPureCloneable())
            return false;
        prefix.push_back(inst);
    }
    if (!guard || guard->getNumResults() != 0)
        return false;
    if (!selectedRegionIsSimple(guard, true) ||
        !selectedRegionIsSimple(guard, false))
        return false;
    if (hasOpaqueCall(guard->getThenRegion()) ||
        (guard->getElseRegion() && hasOpaqueCall(guard->getElseRegion())))
        return false;

    bool thenPureContinue = regionIsPureContinue(guard->getThenRegion());
    bool elsePureContinue = regionIsPureContinue(guard->getElseRegion());
    if (thenPureContinue && elsePureContinue)
        return false;

    if (!(thenPureContinue && !guard->getElseRegion())) {
        for (bool takeThen : {true, false}) {
            Region* branch = takeThen ? guard->getThenRegion() : guard->getElseRegion();
            if (regionIsPureContinue(branch) || !branch)
                continue;
            auto* branchBB = branch->getEntryBlock();
            if (!branchBB)
                return false;
            for (auto* inst : branchBB->getInstructions()) {
                if (inst->isTerminatingFlow())
                    return false;
            }
        }
    }

    Value* ivAddr = fi->getIVAddr();
    ICmpInst::CmpOp pred;
    IVExpr ivExpr;
    Value* bound = nullptr;
    if (!matchIneq(guard->getOperand(0), ivAddr, pred, ivExpr, bound))
        return false;

    if (!loadsAreStableInFor(fi, ivAddr, guard, prefix))
        return false;
    if (!splitCostIsSmall(fi, guard, prefix, suffix))
        return false;

    bool trueIsEarly = false;
    if (!guardTrueIsEarly(pred, increasing, trueIsEarly))
        return false;

    int splitOffset = 0;
    if (increasing) {
        if (pred == ICmpInst::SLT || pred == ICmpInst::SGE)
            splitOffset = -ivExpr.offset;
        else if (pred == ICmpInst::SLE || pred == ICmpInst::SGT)
            splitOffset = 1 - ivExpr.offset;
        else
            return false;
    } else {
        if (pred == ICmpInst::SGT || pred == ICmpInst::SLE)
            splitOffset = -ivExpr.offset;
        else if (pred == ICmpInst::SGE || pred == ICmpInst::SLT)
            splitOffset = -1 - ivExpr.offset;
        else
            return false;
    }

    mi.guard = guard;
    mi.splitBase = bound;
    mi.splitOffset = splitOffset;
    mi.step = step;
    mi.increasing = increasing;
    mi.firstTakesThen = trueIsEarly;
    mi.prefix = std::move(prefix);
    mi.suffix = std::move(suffix);
    return true;
}

bool LoopUnswitch::fillSplitBody(
        ForInst* loop,
        IfInst* guard,
        bool takeThen,
        const ForMatchInfo& mi) {
    // Clone the original body, then inline exactly one side of the guard.
    std::map<Value*, Value*> vmap;
    auto* srcBB = guard ? guard->getParent() : nullptr;
    auto* dstBB = IRRewriter::makeBlock(loop->getBodyRegion(), "for.body");
    if (!srcBB || !dstBB) return false;

    for (auto* inst : srcBB->getInstructions()) {
        if (isa<FlowInst>(inst))
            continue;
        auto* cloned = inst->clone(vmap);
        if (!cloned)
            return false;
        cloned->setName(inst->getName());
        IRRewriter::appendInst(dstBB, cloned);
        if (!cloned->getType()->isVoid())
            vmap[inst] = cloned;
    }

    auto it = vmap.find(guard);
    auto* clonedGuard = it == vmap.end() ? nullptr : dyn_cast<IfInst>(it->second);
    if (!clonedGuard)
        return false;
    return IRRewriter::inlineSelectedBranch(clonedGuard, takeThen);
}

bool LoopUnswitch::applyLoopSplit(const LoopSplitPlan& plan) {
    ForInst* fi = plan.source;
    const ForMatchInfo& mi = plan.match;
    if (!fi || !mi.guard)
        return false;

    auto* parent = fi->getParent();
    if (!parent)
        return false;

    auto& insts = parent->getInstructions();
    auto pos = IRRewriter::findInst(parent, fi);
    if (pos == insts.end())
        return false;

    std::map<Value*, Value*> preLoopMap;
    Value* splitBase = matBefore(
            mi.splitBase, mi.guard->getParent(),
            parent, pos, preLoopMap);
    Value* splitPoint = addConst(splitBase, mi.splitOffset, parent, pos, "us.split");

    // Split into the before-boundary and after-boundary loops.
    bool firstWork = pathHasWork(mi.guard, mi.firstTakesThen, mi.suffix);
    bool secondWork = pathHasWork(mi.guard, !mi.firstTakesThen, mi.suffix);
    if (!firstWork && !secondWork)
        return false;

    ForInst* first = nullptr;
    ForInst* second = nullptr;

    if (firstWork) {
        Value* firstStop = IRRewriter::buildMinOrMax(
                fi->getStop(), splitPoint,
                /*wantMin=*/mi.increasing,
                parent, pos);
        first = IRRewriter::makeFor(
                fi->getStart(), firstStop, fi->getStep(),
                fi->getIVAddr(), fi->getPred(),
                fi->getName() + ".us0");
        if (!fillSplitBody(first, mi.guard, mi.firstTakesThen, mi))
            return false;
    }

    if (secondWork) {
        Value* secondStart = alignStart(
                fi->getStart(), splitPoint, mi.step,
                mi.increasing, parent, pos, "us.align");
        second = IRRewriter::makeFor(
                secondStart, fi->getStop(), fi->getStep(),
                fi->getIVAddr(), fi->getPred(),
                fi->getName() + ".us1");
        if (!fillSplitBody(second, mi.guard, !mi.firstTakesThen, mi))
            return false;
    }

    if (first)
        IRRewriter::insertInst(parent, pos, first);
    if (second)
        IRRewriter::insertInst(parent, pos, second);
    IRRewriter::eraseOp(fi);
    return true;
}

bool LoopUnswitch::runIV() {
    bool any = false;
    for (auto* f : M->getFunctions())
        any |= runIVFunc(f);
    return any;
}

bool LoopUnswitch::runIVFunc(Function* f) {
    if (!f || f->getBody()->getBlocks().empty())
        return false;
    return processIVRegion(f->getBody());
}

bool LoopUnswitch::processIVRegion(Region* region) {
    return IRRewriter::rewriteRegion(region, [&](Instruction* inst) {
        auto* fi = dyn_cast<ForInst>(inst);
        return fi && processIVFor(fi);
    });
}

bool LoopUnswitch::processIVFor(ForInst* fi) {
    ForMatchInfo mi;
    if (!findForMatch(fi, mi))
        return false;
    return applyLoopSplit({fi, std::move(mi)});
}

bool LoopUnswitch::isFlatCloneableForUnroll(Instruction* inst) {
    switch (inst->getOpID()) {
        case Instruction::Add: case Instruction::Sub:
        case Instruction::Mul: case Instruction::Div: case Instruction::Mod:
        case Instruction::Shl: case Instruction::Ashr: case Instruction::And:
        case Instruction::FAdd: case Instruction::FSub:
        case Instruction::FMul: case Instruction::FDiv:
        case Instruction::ICmp: case Instruction::FCmp:
        case Instruction::SIToFP: case Instruction::FPToSI:
        case Instruction::Alloca:
        case Instruction::Load:
        case Instruction::Store:
        case Instruction::GetElementPtr:
        case Instruction::Call:
        case Instruction::Flow:
            return true;
        default:
            return false;
    }
}

bool LoopUnswitch::matchMod(ForInst* fi, ModMatchInfo& mi) {
    // Match periodic guards such as if (i % A == c).
    auto* stepConst = dyn_cast<ConstantInt>(fi->getStep());
    if (!stepConst) return false;
    int stepVal = stepConst->getValue();
    if (stepVal <= 0) return false;
    if (fi->getPred() != ICmpInst::SLT) return false;

    auto* startConst = dyn_cast<ConstantInt>(fi->getStart());
    if (!startConst) return false;

    auto* bodyBB = fi->getBodyRegion()->getEntryBlock();
    if (!bodyBB) return false;

    IfInst* guard = nullptr;
    for (auto* inst : bodyBB->getInstructions()) {
        if (auto* ii = dyn_cast<IfInst>(inst)) { guard = ii; break; }
        if (inst->isTerminatingFlow() ||
            inst->getOpID() == Instruction::If ||
            inst->getOpID() == Instruction::While ||
            inst->getOpID() == Instruction::For ||
            !isFlatCloneableForUnroll(inst))
            return false;
    }
    if (!guard) return false;

    Value* ivAddr = fi->getIVAddr();
    int A = 0;

    auto raw = matchIVModExpr(guard->getOperand(0), ivAddr);
    if (raw.matched) {
        if (!absDivisor(raw.divisor, A))
            return false;
    } else {
        auto* cmpInst = dyn_cast<ICmpInst>(guard->getOperand(0));
        if (!cmpInst) return false;
        bool isEq;
        if (cmpInst->getPredicate() == ICmpInst::EQ) isEq = true;
        else if (cmpInst->getPredicate() == ICmpInst::NE) isEq = false;
        else return false;

        static Pattern eqDirect("(eq (mod x 'a) 'b)");
        static Pattern eqReversed("(eq 'b (mod x 'a))");
        static Pattern neDirect("(ne (mod x 'a) 'b)");
        static Pattern neReversed("(ne 'b (mod x 'a))");

        Pattern* p1 = isEq ? &eqDirect : &neDirect;
        Pattern* p2 = isEq ? &eqReversed : &neReversed;

        auto tryPat = [&](Pattern& pat) -> bool {
            if (!pat.match(cmpInst)) return false;
            int aVal = 0, bVal = 0;
            if (!pat.extractInt("'a", aVal) || !pat.extractInt("'b", bVal))
                return false;
            auto iv = matchIVExpr(pat.extract("x"), ivAddr);
            if (!iv.matched) return false;
            if (!absDivisor(aVal, aVal)) return false;
            if (bVal < 0 || bVal >= aVal) return false;
            A = aVal;
            return true;
        };

        if (!tryPat(*p1) && !tryPat(*p2)) return false;
    }

    if (A < 2) return false;

    long long groupStep = 1LL * A * stepVal;
    if (groupStep > INT_MAX || groupStep < INT_MIN)
        return false;

    mi.A = A;
    mi.startVal = startConst->getValue();
    mi.stepVal = stepVal;
    return true;
}

void LoopUnswitch::tidyMod(ForInst* fi, int vi, int startVal, int stepVal) {
    Value* ivAddr = fi->getIVAddr();
    int period = vi * stepVal;
    tidyModRegion(fi->getBodyRegion(), ivAddr, period, startVal);
}

void LoopUnswitch::tidyModRegion(Region* region, Value* ivAddr, int period, int startVal) {
    if (!region)
        return;

    for (auto* bb : region->getBlocks()) {
        std::vector<Instruction*> toErase;
        for (auto* inst : bb->getInstructions()) {
            if (auto* ii = dyn_cast<IfInst>(inst)) {
                tidyModRegion(ii->getThenRegion(), ivAddr, period, startVal);
                if (ii->getElseRegion())
                    tidyModRegion(ii->getElseRegion(), ivAddr, period, startVal);
            } else if (auto* nestedFor = dyn_cast<ForInst>(inst)) {
                tidyModRegion(nestedFor->getBodyRegion(), ivAddr, period, startVal);
            } else if (auto* wi = dyn_cast<WhileInst>(inst)) {
                tidyModRegion(wi->getCondRegion(), ivAddr, period, startVal);
                tidyModRegion(wi->getBodyRegion(), ivAddr, period, startVal);
            }

            auto* modInst = dyn_cast<BinaryInst>(inst);
            if (!modInst || modInst->getOpID() != Instruction::Mod)
                continue;

            auto* divConst = dyn_cast<ConstantInt>(modInst->getOperand(1));
            if (!divConst || divConst->getValue() <= 0)
                continue;
            int divisor = divConst->getValue();
            if (period % divisor != 0)
                continue;

            // After unrolling by the period, each clone has a constant remainder.
            IVExpr expr = matchIVExpr(modInst->getOperand(0), ivAddr);
            if (!expr.matched)
                continue;

            int result = ((startVal + expr.offset) % divisor + divisor) % divisor;
            auto* constResult = new ConstantInt(result);
            modInst->replaceAllUsesWith(constResult);
            toErase.push_back(modInst);
        }
        for (auto* inst : toErase)
            IRRewriter::eraseOp(inst);
    }
}

void LoopUnswitch::subIVLoads(Region* region, Value* ivAddr, Value* ivZ) {
    if (!region) return;
    for (auto* bb : region->getBlocks()) {
        std::vector<Instruction*> toErase;
        for (auto* inst : bb->getInstructions()) {
            if (auto* ld = dyn_cast<LoadInst>(inst)) {
                if (ld->getOperand(0) == ivAddr) {
                    ld->replaceAllUsesWith(ivZ);
                    toErase.push_back(ld);
                }
            } else if (auto* ii = dyn_cast<IfInst>(inst)) {
                subIVLoads(ii->getThenRegion(), ivAddr, ivZ);
                if (ii->getElseRegion())
                    subIVLoads(ii->getElseRegion(), ivAddr, ivZ);
            } else if (auto* fi2 = dyn_cast<ForInst>(inst)) {
                subIVLoads(fi2->getBodyRegion(), ivAddr, ivZ);
            } else if (auto* wi2 = dyn_cast<WhileInst>(inst)) {
                subIVLoads(wi2->getCondRegion(), ivAddr, ivZ);
                subIVLoads(wi2->getBodyRegion(), ivAddr, ivZ);
            }
        }
        for (auto* inst : toErase)
            IRRewriter::eraseOp(inst);
    }
}

void LoopUnswitch::subIVLoadsInClone(Instruction* inst, Value* ivAddr, Value* ivZ) {
    if (auto* ii = dyn_cast<IfInst>(inst)) {
        subIVLoads(ii->getThenRegion(), ivAddr, ivZ);
        if (ii->getElseRegion())
            subIVLoads(ii->getElseRegion(), ivAddr, ivZ);
    } else if (auto* fi = dyn_cast<ForInst>(inst)) {
        subIVLoads(fi->getBodyRegion(), ivAddr, ivZ);
    } else if (auto* wi = dyn_cast<WhileInst>(inst)) {
        subIVLoads(wi->getCondRegion(), ivAddr, ivZ);
        subIVLoads(wi->getBodyRegion(), ivAddr, ivZ);
    }
}

void LoopUnswitch::cloneBodyInto(
        BasicBlock* targetBB,
        const std::vector<Instruction*>& body,
        std::map<Value*, Value*>& vmap,
        const std::string& suffix,
        Value* ivAddr,
        Value* ivValue) {
    for (auto* inst : body) {
        if (vmap.count(inst))
            continue;

        Instruction* cloned = inst->clone(vmap);
        if (cloned && ivAddr && ivValue &&
            (inst->getOpID() == Instruction::If ||
             inst->getOpID() == Instruction::While ||
             inst->getOpID() == Instruction::For))
            subIVLoadsInClone(cloned, ivAddr, ivValue);

        if (!cloned)
            continue;
        cloned->setName(inst->getName() + suffix);
        IRRewriter::appendInst(targetBB, cloned);
        if (!cloned->getType()->isVoid())
            vmap[inst] = cloned;
    }
}

void LoopUnswitch::cloneUnrolledBody(
        ForInst* fi,
        const std::vector<Instruction*>& body,
        Value* ivBase,
        int vi,
        int stepVal) {
    Value* ivAddr = fi->getIVAddr();
    auto* bodyBB = fi->getBodyRegion()->getEntryBlock();

    for (int z = 1; z < vi; z++) {
        // Clone body z sees IV as base + z * step.
        int delta = z * stepVal;
        auto* ivZ = new BinaryInst(
                Instruction::Add, ivBase,
                new ConstantInt(delta), nullptr);
        ivZ->setName("mus.iv" + std::to_string(z));
        IRRewriter::appendInst(bodyBB, ivZ);

        std::map<Value*, Value*> vmap;
        for (auto* inst : body) {
            if (auto* ld = dyn_cast<LoadInst>(inst)) {
                if (ld->getOperand(0) == ivAddr)
                    vmap[ld] = ivZ;
            }
        }

        cloneBodyInto(bodyBB, body, vmap, ".mu" + std::to_string(z), ivAddr, ivZ);
    }
}

Value* LoopUnswitch::buildAlignedEnd(
        ForInst* fi, int vi, int startVal,
        int stepVal,
        BasicBlock* parentBB,
        std::list<Instruction*>::iterator pos) {
    Value* origStop = fi->getStop();
    if (auto* stopConst = dyn_cast<ConstantInt>(origStop)) {
        int stopVal = stopConst->getValue();
        long long diff = 1LL * stopVal - startVal;
        long long trip = diff <= 0 ? 0 : (diff + stepVal - 1) / stepVal;
        long long groups = trip / vi;
        long long end = 1LL * startVal + groups * vi * stepVal;
        if (end >= INT_MIN && end <= INT_MAX)
            return new ConstantInt(static_cast<int>(end));
    }

    auto* startC = new ConstantInt(startVal);
    auto* viC = new ConstantInt(vi);
    auto* stepC = new ConstantInt(stepVal);
    auto* zeroC = new ConstantInt(0);

    IRBuilder builder;
    builder.SetInsertPoint(parentBB, pos);

    auto* sub = builder.InsertNew<BinaryInst>(Instruction::Sub, origStop, startC);
    auto* stepMinusOne = builder.InsertNew<BinaryInst>(Instruction::Sub, stepC, new ConstantInt(1));
    auto* ceilNum = builder.InsertNew<BinaryInst>(Instruction::Add, sub, stepMinusOne);
    auto* trip = builder.InsertNew<BinaryInst>(Instruction::Div, ceilNum, stepC);

    auto* nonNegTrip = IRRewriter::buildMinOrMax(trip, zeroC, false, parentBB, pos);

    auto* groups = builder.InsertNew<BinaryInst>(Instruction::Div, nonNegTrip, viC);
    groups->setName("mus.groups");

    auto* groupIters = builder.InsertNew<BinaryInst>(Instruction::Mul, groups, viC);
    auto* span = builder.InsertNew<BinaryInst>(Instruction::Mul, groupIters, stepC);

    auto* alignedEnd = builder.InsertNew<BinaryInst>(Instruction::Add, startC, span);
    alignedEnd->setName("mus.ae");
    return alignedEnd;
}

ForInst* LoopUnswitch::buildSideLoop(
        ForInst* fi,
        const std::vector<Instruction*>& body,
        Value* alignedEnd,
        Value* origStop,
        int stepVal,
        BasicBlock* parentBB,
        std::list<Instruction*>::iterator pos) {
    auto* sideFi = IRRewriter::makeFor(
            alignedEnd, origStop, new ConstantInt(stepVal),
            fi->getIVAddr(), ICmpInst::SLT, fi->getName() + ".side");
    IRRewriter::insertInst(parentBB, std::next(pos), sideFi);

    auto* sideBB = IRRewriter::makeBlock(sideFi->getBodyRegion(), "side_body");
    std::map<Value*, Value*> sideVmap;
    cloneBodyInto(sideBB, body, sideVmap, ".side");

    return sideFi;
}

ForInst* LoopUnswitch::unrollFor(
        ForInst* fi, int vi, int startVal,
        int stepVal,
        BasicBlock* parentBB,
        std::list<Instruction*>::iterator pos) {
    auto* bodyBB = fi->getBodyRegion()->getEntryBlock();
    std::vector<Instruction*> body(
            bodyBB->getInstructions().begin(),
            bodyBB->getInstructions().end());
    Value* origStop = fi->getStop();

    IRBuilder bodyBuilder;
    bodyBuilder.SetInsertPoint(bodyBB, bodyBB->getInstructions().begin());
    auto* ivBase = bodyBuilder.InsertNew<LoadInst>(fi->getIVAddr());
    ivBase->setName("mus.iv");

    cloneUnrolledBody(fi, body, ivBase, vi, stepVal);

    Value* alignedEnd = buildAlignedEnd(fi, vi, startVal, stepVal, parentBB, pos);
    int groupStep = vi * stepVal;
    fi->setOperand(1, alignedEnd);
    fi->setOperand(2, new ConstantInt(groupStep));

    return buildSideLoop(fi, body, alignedEnd, origStop, stepVal, parentBB, pos);
}

bool LoopUnswitch::runMod() {
    bool any = false;
    for (auto* f : M->getFunctions())
        any |= runModFunc(f);
    return any;
}

bool LoopUnswitch::runModFunc(Function* f) {
    if (!f || f->getBody()->getBlocks().empty()) return false;
    return processModRegion(f->getBody());
}

bool LoopUnswitch::processModRegion(Region* region) {
    return IRRewriter::rewriteRegion(region, [&](Instruction* inst) {
        auto* fi = dyn_cast<ForInst>(inst);
        return fi && processModFor(fi);
    });
}

bool LoopUnswitch::processModFor(ForInst* fi) {
    if (sideLoops_.count(fi)) return false;

    ModMatchInfo mi;
    if (!matchMod(fi, mi)) return false;

    auto* parent = fi->getParent();
    if (!parent) return false;
    auto& insts = parent->getInstructions();
    auto pos = IRRewriter::findInst(parent, fi);
    if (pos == insts.end()) return false;

    ForInst* sideLoop = unrollFor(fi, mi.A, mi.startVal, mi.stepVal, parent, pos);
    if (sideLoop)
        sideLoops_.insert(sideLoop);

    tidyMod(fi, mi.A, mi.startVal, mi.stepVal);
    return true;
}
