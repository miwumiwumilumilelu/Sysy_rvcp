#include "IR/IRRewriter.h"
#include "IR/Module.h"
#include <algorithm>

namespace sysy {

static void dropNestedUses(Instruction* inst) {
    if (!inst)
        return;

    for (auto& region : inst->getRegions()) {
        if (!region)
            continue;
        for (auto* bb : region->getBlocks()) {
            std::vector<Instruction*> insts(bb->getInstructions().begin(),
                                            bb->getInstructions().end());
            for (auto* nested : insts) {
                dropNestedUses(nested);
                nested->replaceAllUsesWith(nullptr);
                nested->dropAllOperands();
            }
        }
    }
}

IRRewriter::InstIter IRRewriter::findInst(BasicBlock* bb, Instruction* inst) {
    if (!bb) return {};
    auto& list = bb->getInstructions();
    return std::find(list.begin(), list.end(), inst);
}

void IRRewriter::eraseOp(Instruction* inst) {
    if (!inst) return;
    inst->replaceAllUsesWith(nullptr);
    dropNestedUses(inst);
    inst->eraseInst();
}

void IRRewriter::replaceAndErase(Instruction* inst, Value* newVal) {
    inst->replaceAllUsesWith(newVal);
    inst->eraseInst();
}

void IRRewriter::moveInstBefore(Instruction* inst, Instruction* anchor) {
    BasicBlock* srcBB = inst->getParent();
    BasicBlock* dstBB = anchor->getParent();
    if (srcBB)
        srcBB->getInstructions().remove(inst);
    inst->setParent(dstBB);
    auto& list = dstBB->getInstructions();
    auto it = std::find(list.begin(), list.end(), anchor);
    list.insert(it, inst);
}

void IRRewriter::moveInstAfter(Instruction* inst, Instruction* anchor) {
    BasicBlock* srcBB = inst->getParent();
    BasicBlock* dstBB = anchor->getParent();
    if (srcBB)
        srcBB->getInstructions().remove(inst);
    inst->setParent(dstBB);
    auto& list = dstBB->getInstructions();
    auto it = std::find(list.begin(), list.end(), anchor);
    ++it;
    list.insert(it, inst);
}

void IRRewriter::inlineRegionBefore(Region* src, Instruction* anchor) {
    BasicBlock* dstBB = anchor->getParent();
    auto& dstList = dstBB->getInstructions();
    auto anchorIt = std::find(dstList.begin(), dstList.end(), anchor);

    std::vector<BasicBlock*> blocks(src->getBlocks().begin(), src->getBlocks().end());
    for (auto* bb : blocks) {
        auto& srcList = bb->getInstructions();
        for (auto* inst : srcList)
            inst->setParent(dstBB);
        dstList.splice(anchorIt, srcList);
        src->removeBlock(bb);
    }
}

static bool walkImpl(Region* r, const std::function<bool(Instruction*)>& fn) {
    for (auto* bb : r->getBlocks()) {
        std::vector<Instruction*> snap(bb->getInstructions().begin(),
                                       bb->getInstructions().end());
        for (auto* inst : snap) {
            if (!fn(inst)) return false;
            for (auto& reg : inst->getRegions())
                if (!walkImpl(reg.get(), fn)) return false;
        }
    }
    return true;
}

static bool walkPostImpl(Region* r, const std::function<bool(Instruction*)>& fn) {
    for (auto* bb : r->getBlocks()) {
        std::vector<Instruction*> snap(bb->getInstructions().begin(),
                                       bb->getInstructions().end());
        for (auto* inst : snap) {
            for (auto& reg : inst->getRegions())
                if (!walkPostImpl(reg.get(), fn)) return false;
            if (!fn(inst)) return false;
        }
    }
    return true;
}

bool IRRewriter::walk(Region* r, const std::function<bool(Instruction*)>& fn) {
    return walkImpl(r, fn);
}

bool IRRewriter::walk(Function* f, const std::function<bool(Instruction*)>& fn) {
    return walkImpl(f->getBody(), fn);
}

bool IRRewriter::walkPost(Region* r, const std::function<bool(Instruction*)>& fn) {
    return walkPostImpl(r, fn);
}

bool IRRewriter::walkPost(Function* f, const std::function<bool(Instruction*)>& fn) {
    return walkPostImpl(f->getBody(), fn);
}

BasicBlock* IRRewriter::makeBlock(Region* region, const std::string& name) {
    return new BasicBlock(name, region);
}

void IRRewriter::appendInst(BasicBlock* bb, Instruction* inst) {
    if (!inst) return;
    inst->setParent(bb);
    bb->getInstructions().push_back(inst);
}

FlowInst* IRRewriter::appendFlow(BasicBlock* bb, const std::vector<Value*>& vals) {
    auto* f = new FlowInst(vals, nullptr);
    appendInst(bb, f);
    return f;
}

void IRRewriter::insertInst(BasicBlock* bb,
                            std::list<Instruction*>::iterator pos,
                            Instruction* inst) {
    if (!inst) return;
    inst->setParent(bb);
    bb->getInstructions().insert(pos, inst);
}

IfInst* IRRewriter::makeIf(Value* cond, const std::string& name, bool hasElse) {
    auto* inst = new IfInst(cond, nullptr);
    inst->setName(name);
    if (hasElse) inst->addElseRegion();
    return inst;
}

ForInst* IRRewriter::makeFor(Value* start, Value* stop, Value* step, Value* ivAddr,
                             ICmpInst::CmpOp pred, const std::string& name) {
    auto* inst = new ForInst(start, stop, step, ivAddr, pred, nullptr);
    inst->setName(name);
    return inst;
}

Value* IRRewriter::buildMinOrMax(Value* a, Value* b, bool wantMin,
                                 BasicBlock* parentBB,
                                 std::list<Instruction*>::iterator pos) {
    auto* ac = dyn_cast<ConstantInt>(a);
    auto* bc = dyn_cast<ConstantInt>(b);
    if (ac && bc) {
        int va = ac->getValue(), vb = bc->getValue();
        return new ConstantInt(wantMin ? std::min(va, vb) : std::max(va, vb));
    }
    if (a == b) return a;

    IRBuilder builder;
    builder.SetInsertPoint(parentBB, pos);

    auto* cmp = builder.InsertNew<ICmpInst>(wantMin ? ICmpInst::SLT : ICmpInst::SGT, a, b);
    cmp->setName(wantMin ? "rw.min.cmp" : "rw.max.cmp");

    auto* sel = builder.InsertNew<IfInst>(cmp);
    sel->setName(wantMin ? "rw.min" : "rw.max");
    sel->addElseRegion();
    auto* result = sel->createResult(Type::getIntTy());
    result->setName(wantMin ? "rw.min.v" : "rw.max.v");

    auto* thenBB = makeBlock(sel->getThenRegion(), wantMin ? "min.t" : "max.t");
    appendFlow(thenBB, {a});

    auto* elseBB = makeBlock(sel->getElseRegion(), wantMin ? "min.e" : "max.e");
    appendFlow(elseBB, {b});

    return result;
}

void IRRewriter::pruneAfterTerminatingFlow(BasicBlock* bb) {
    if (!bb) return;
    auto& insts = bb->getInstructions();
    bool seenTerm = false;
    std::vector<Instruction*> dead;
    for (auto* inst : insts) {
        if (seenTerm) dead.push_back(inst);
        if (inst->isTerminatingFlow()) seenTerm = true;
    }
    for (auto* inst : dead) eraseOp(inst);
}

bool IRRewriter::inlineSelectedBranch(IfInst* guard, bool takeThen) {
    if (!guard || !guard->getParent()) return false;
    Region* selected = takeThen ? guard->getThenRegion() : guard->getElseRegion();
    BasicBlock* parent = guard->getParent();
    auto& insts = parent->getInstructions();
    auto pos = std::find(insts.begin(), insts.end(), static_cast<Instruction*>(guard));
    if (pos == insts.end()) return false;

    if (selected) {
        if (selected->getBlocks().size() != 1) return false;
        auto* block = selected->getEntryBlock();
        std::map<Value*, Value*> vmap;
        FlowInst* selectedFlow = nullptr;
        for (auto* inst : block->getInstructions()) {
            if (auto* flow = dyn_cast<FlowInst>(inst)) {
                selectedFlow = flow;
                break;
            }
            auto* cloned = inst->clone(vmap);
            if (!cloned) return false;
            cloned->setName(inst->getName());
            cloned->setParent(parent);
            insts.insert(pos, cloned);
            if (!cloned->getType()->isVoid())
                vmap[inst] = cloned;
        }
        if (guard->getNumResults() != 0) {
            if (!selectedFlow || selectedFlow->getNumValues() < guard->getNumResults())
                return false;
            std::map<BasicBlock*, BasicBlock*> emptyBBMap;
            for (unsigned i = 0; i < guard->getNumResults(); ++i) {
                Value* val = vmap.count(selectedFlow->getValue(i))
                                 ? vmap[selectedFlow->getValue(i)]
                                 : selectedFlow->getValue(i);
                guard->getResult(i)->replaceAllUsesWith(val);
            }
        }
    } else if (guard->getNumResults() != 0) {
        return false;
    }

    eraseOp(guard);
    pruneAfterTerminatingFlow(parent);
    return true;
}

bool IRRewriter::cloneForBody(const std::vector<Instruction*>& prefix,
                              Region* selectedRegion,
                              const std::vector<Instruction*>& suffix,
                              bool includeSuffix,
                              Region* targetRegion,
                              std::map<Value*, Value*>& vmap) {
    auto* targetBB = new BasicBlock("for.body", targetRegion);

    auto cloneOne = [&](Instruction* inst) -> bool {
        if (isa<FlowInst>(inst) || isa<ContinueInst>(inst)) return true;
        auto* cloned = inst->clone(vmap);
        if (!cloned) return false;
        cloned->setName(inst->getName());
        appendInst(targetBB, cloned);
        if (!cloned->getType()->isVoid())
            vmap[inst] = cloned;
        return true;
    };

    for (auto* inst : prefix)
        if (!cloneOne(inst)) return false;

    if (selectedRegion && !selectedRegion->getBlocks().empty()) {
        if (selectedRegion->getBlocks().size() != 1) return false;
        for (auto* inst : selectedRegion->getEntryBlock()->getInstructions())
            if (!cloneOne(inst)) return false;
    }

    if (includeSuffix)
        for (auto* inst : suffix)
            if (!cloneOne(inst)) return false;

    return true;
}

} // namespace sysy
