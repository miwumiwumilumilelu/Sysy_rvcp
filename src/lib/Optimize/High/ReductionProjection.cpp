#include "../../../include/Optimize/High/ReductionProjection.h"
#include "../../../include/IR/IRRewriter.h"
#include <algorithm>
#include <set>
#include <vector>

using namespace sysy;

namespace {

struct Access {
    Value* root = nullptr;
    std::vector<Value*> indices;
};

static Value* uniqueStoredValue(AllocaInst* slot) {
    Value* result = nullptr;
    for (auto* user : slot->getUsers()) {
        if (auto* load = dyn_cast<LoadInst>(user)) {
            if (load->getOperand(0) == slot) continue;
        }
        auto* store = dyn_cast<StoreInst>(user);
        if (!store || store->getOperand(1) != slot || result)
            return nullptr;
        result = store->getOperand(0);
    }
    return result;
}

static Value* stripPointerLoads(Value* value) {
    std::set<Value*> seen;
    while (value && seen.insert(value).second) {
        auto* load = dyn_cast<LoadInst>(value);
        if (!load) break;
        auto* slot = dyn_cast<AllocaInst>(load->getOperand(0));
        if (!slot) break;
        Value* stored = uniqueStoredValue(slot);
        if (!stored) break;
        value = stored;
    }
    return value;
}

static Access decomposeAccess(Value* ptr) {
    Access result;
    std::vector<Value*> reverse;
    Value* current = ptr;
    while (auto* gep = dyn_cast<GetElementPtrInst>(current)) {
        reverse.push_back(gep->getOperand(1));
        current = stripPointerLoads(gep->getOperand(0));
    }
    result.root = stripPointerLoads(current);
    result.indices.assign(reverse.rbegin(), reverse.rend());
    return result;
}

static bool isLoadOf(Value* value, Value* address) {
    auto* load = dyn_cast<LoadInst>(value);
    return load && load->getOperand(0) == address;
}

static bool isZero(Value* value) {
    if (auto* c = dyn_cast<ConstantInt>(value)) return c->getValue() == 0;
    return isa<ConstantZero>(value);
}

static bool isOne(Value* value) {
    auto* c = dyn_cast<ConstantInt>(value);
    return c && c->getValue() == 1;
}

static bool isProjectedIndex(Value* value, ForInst* loop) {
    return isLoadOf(value, loop->getIVAddr());
}

static bool sameIndex(Value* a, Value* b) {
    if (a == b) return true;
    auto* la = dyn_cast<LoadInst>(a);
    auto* lb = dyn_cast<LoadInst>(b);
    return la && lb && la->getOperand(0) == lb->getOperand(0);
}

static bool sameAccess(const Access& a, const Access& b) {
    if (a.root != b.root || a.indices.size() != b.indices.size()) return false;
    for (size_t i = 0; i < a.indices.size(); ++i)
        if (!sameIndex(a.indices[i], b.indices[i])) return false;
    return true;
}

static void collectRegion(Region* region, std::vector<Instruction*>& out) {
    if (!region) return;
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            out.push_back(inst);
            for (auto& nested : inst->getRegions())
                collectRegion(nested.get(), out);
        }
    }
}

static std::vector<Instruction*> directInstructions(ForInst* loop) {
    std::vector<Instruction*> result;
    if (!loop || !loop->getBodyRegion()) return result;
    for (auto* bb : loop->getBodyRegion()->getBlocks())
        result.insert(result.end(), bb->getInstructions().begin(),
                      bb->getInstructions().end());
    return result;
}

static bool isInsideLoop(Instruction* inst, ForInst* loop) {
    if (!inst || !loop) return false;
    Region* region = inst->getParent() ? inst->getParent()->getParent() : nullptr;
    while (region) {
        Instruction* parent = region->getParentInst();
        if (parent == loop) return true;
        region = parent && parent->getParent()
                     ? parent->getParent()->getParent()
                     : nullptr;
    }
    return false;
}

static bool valueDependsOn(Value* value, Value* address,
                           std::set<Value*>& visiting) {
    if (!value || !visiting.insert(value).second) return false;
    if (isLoadOf(value, address)) return true;
    auto* inst = dyn_cast<Instruction>(value);
    if (!inst) return false;
    for (int i = 0; i < inst->getNumOperands(); ++i)
        if (valueDependsOn(inst->getOperand(i), address, visiting)) return true;
    return false;
}

static bool valueDependsOn(Value* value, Value* address) {
    std::set<Value*> visiting;
    return valueDependsOn(value, address, visiting);
}

struct KernelInfo {
    Function* function = nullptr;
    Argument* coefficient = nullptr;
    Argument* source = nullptr;
    Argument* destination = nullptr;
    ForInst* clearProjectedLoop = nullptr;
    ForInst* updateProjectedLoop = nullptr;
};

static bool matchClearLoop(ForInst* loop, Argument* destination) {
    if (!loop || !isZero(loop->getStart()) || !isOne(loop->getStep())) return false;
    StoreInst* store = nullptr;
    std::vector<Instruction*> all;
    collectRegion(loop->getBodyRegion(), all);
    for (auto* inst : all) {
        if (auto* candidate = dyn_cast<StoreInst>(inst)) {
            if (store) return false;
            store = candidate;
        }
        if (isa<CallInst>(inst) || isa<BreakInst>(inst) || isa<ContinueInst>(inst))
            return false;
    }
    if (!store || !isZero(store->getOperand(0))) return false;
    Access dst = decomposeAccess(store->getOperand(1));
    return dst.root == destination && dst.indices.size() == 2 &&
           isProjectedIndex(dst.indices.back(), loop);
}

static bool matchUpdateLoop(ForInst* loop, Argument* coefficient,
                            Argument* source, Argument* destination) {
    if (!loop || !isZero(loop->getStart()) || !isOne(loop->getStep())) return false;
    StoreInst* store = nullptr;
    std::vector<Instruction*> all;
    collectRegion(loop->getBodyRegion(), all);
    for (auto* inst : all) {
        if (auto* candidate = dyn_cast<StoreInst>(inst)) {
            if (store) return false;
            store = candidate;
        }
        if (isa<CallInst>(inst) || isa<BreakInst>(inst) || isa<ContinueInst>(inst) ||
            isa<IfInst>(inst) || isa<WhileInst>(inst) || isa<ForInst>(inst))
            return false;
    }
    if (!store) return false;
    Access dst = decomposeAccess(store->getOperand(1));
    if (dst.root != destination || dst.indices.size() != 2 ||
        !isProjectedIndex(dst.indices.back(), loop))
        return false;

    auto* add = dyn_cast<BinaryInst>(store->getOperand(0));
    if (!add || add->getOpID() != Instruction::Add) return false;
    BinaryInst* mul = nullptr;
    LoadInst* sourceLoad = nullptr;
    for (int order = 0; order < 2; ++order) {
        mul = dyn_cast<BinaryInst>(add->getOperand(order));
        sourceLoad = dyn_cast<LoadInst>(add->getOperand(1 - order));
        if (mul && mul->getOpID() == Instruction::Mul && sourceLoad) break;
        mul = nullptr;
        sourceLoad = nullptr;
    }
    if (!mul || !sourceLoad) return false;
    Access src = decomposeAccess(sourceLoad->getOperand(0));
    if (src.root != source || src.indices.size() != 2 ||
        !sameIndex(src.indices.back(), dst.indices.back()))
        return false;

    LoadInst* dstLoad = nullptr;
    Value* uniform = nullptr;
    for (int order = 0; order < 2; ++order) {
        auto* candidate = dyn_cast<LoadInst>(mul->getOperand(order));
        if (!candidate) continue;
        if (!sameAccess(decomposeAccess(candidate->getOperand(0)), dst)) continue;
        dstLoad = candidate;
        uniform = mul->getOperand(1 - order);
        break;
    }
    if (!dstLoad || valueDependsOn(uniform, loop->getIVAddr())) return false;

    // The coefficient may be a runtime value, but must originate from the
    // non-projected coefficient argument rather than either projected array.
    bool seesCoefficient = false;
    std::set<Value*> work = {uniform};
    std::set<Value*> visited;
    while (!work.empty()) {
        Value* value = *work.begin();
        work.erase(work.begin());
        if (!visited.insert(value).second) continue;
        Access access;
        if (auto* load = dyn_cast<LoadInst>(value)) {
            access = decomposeAccess(load->getOperand(0));
            if (access.root == coefficient) seesCoefficient = true;
            if (access.root == source || access.root == destination) return false;
        }
        if (auto* inst = dyn_cast<Instruction>(value))
            for (int i = 0; i < inst->getNumOperands(); ++i)
                if (inst->getOperand(i)) work.insert(inst->getOperand(i));
    }
    return seesCoefficient;
}

static bool matchKernel(Function* function, KernelInfo& out) {
    if (!function || function->getArgs().size() != 4 ||
        !function->getType()->isVoid())
        return false;
    auto& args = function->getArgs();
    if (!args[0]->getType()->isInt() || !args[1]->getType()->isPointer() ||
        !args[2]->getType()->isPointer() || !args[3]->getType()->isPointer())
        return false;

    std::vector<Instruction*> all;
    collectRegion(function->getBody(), all);
    std::vector<ForInst*> loops;
    for (auto* inst : all)
        if (auto* loop = dyn_cast<ForInst>(inst)) loops.push_back(loop);

    ForInst* clear = nullptr;
    ForInst* update = nullptr;
    for (auto* loop : loops) {
        if (!clear && matchClearLoop(loop, args[3])) clear = loop;
        if (!update && matchUpdateLoop(loop, args[1], args[2], args[3]))
            update = loop;
    }
    if (!clear || !update || !sameIndex(clear->getStop(), update->getStop()))
        return false;

    // The projected extent must be the function's scalar extent argument.
    if (stripPointerLoads(clear->getStop()) != args[0]) return false;

    // Do not silently discard any other observation or mutation of either
    // projected array inside the kernel.
    for (auto* inst : all) {
        Value* ptr = nullptr;
        if (auto* load = dyn_cast<LoadInst>(inst)) ptr = load->getOperand(0);
        if (auto* store = dyn_cast<StoreInst>(inst)) ptr = store->getOperand(1);
        if (!ptr) continue;
        Value* root = decomposeAccess(ptr).root;
        if (root == args[2] && !isInsideLoop(inst, update)) return false;
        if (root == args[3] && !isInsideLoop(inst, clear) &&
            !isInsideLoop(inst, update))
            return false;
    }
    out = {function, args[1], args[2], args[3], clear, update};
    return true;
}

static GlobalVariable* globalRoot(Value* value) {
    return dyn_cast<GlobalVariable>(decomposeAccess(value).root);
}

struct ReductionInfo {
    ForInst* inner = nullptr;
    GlobalVariable* global = nullptr;
};

static bool matchReductionLoop(ForInst* loop, ReductionInfo& out) {
    if (!loop || !isZero(loop->getStart()) || !isOne(loop->getStep())) return false;
    auto direct = directInstructions(loop);
    StoreInst* store = nullptr;
    for (auto* inst : direct)
        if (auto* candidate = dyn_cast<StoreInst>(inst)) {
            if (store) return false;
            store = candidate;
        }
    if (!store) return false;
    auto* add = dyn_cast<BinaryInst>(store->getOperand(0));
    if (!add || add->getOpID() != Instruction::Add) return false;
    LoadInst* accumulator = nullptr;
    LoadInst* element = nullptr;
    for (int order = 0; order < 2; ++order) {
        accumulator = dyn_cast<LoadInst>(add->getOperand(order));
        element = dyn_cast<LoadInst>(add->getOperand(1 - order));
        if (accumulator && element &&
            accumulator->getOperand(0) == store->getOperand(1)) break;
        accumulator = element = nullptr;
    }
    if (!accumulator || !element) return false;
    Access access = decomposeAccess(element->getOperand(0));
    auto* global = dyn_cast<GlobalVariable>(access.root);
    if (!global || access.indices.size() != 3 ||
        !isProjectedIndex(access.indices.back(), loop))
        return false;
    out = {loop, global};
    return true;
}

struct FillInfo {
    ForInst* inner = nullptr;
    ForInst* outer = nullptr;
    StoreInst* store = nullptr;
    CallInst* input = nullptr;
    GlobalVariable* global = nullptr;
};

static ForInst* parentFor(ForInst* inner) {
    if (!inner || !inner->getParent() || !inner->getParent()->getParent()) return nullptr;
    return dyn_cast<ForInst>(inner->getParent()->getParent()->getParentInst());
}

static bool matchFillLoop(ForInst* loop, GlobalVariable* global, FillInfo& out) {
    if (!loop || !isZero(loop->getStart()) || !isOne(loop->getStep())) return false;
    StoreInst* store = nullptr;
    CallInst* input = nullptr;
    for (auto* inst : directInstructions(loop)) {
        if (auto* candidate = dyn_cast<StoreInst>(inst)) {
            if (store) return false;
            store = candidate;
        } else if (auto* call = dyn_cast<CallInst>(inst)) {
            if (input) return false;
            input = call;
        }
    }
    if (!store || !input || store->getOperand(0) != input ||
        !input->getFunction()->getBody()->getBlocks().empty())
        return false;
    Access access = decomposeAccess(store->getOperand(1));
    if (access.root != global || access.indices.size() != 3 ||
        !isProjectedIndex(access.indices.back(), loop))
        return false;
    auto* outer = parentFor(loop);
    if (!outer || access.indices.size() < 2 ||
        !isProjectedIndex(access.indices[1], outer))
        return false;
    out = {loop, outer, store, input, global};
    return true;
}

static void insertBefore(Instruction* anchor, Instruction* inst) {
    auto* bb = anchor->getParent();
    auto& list = bb->getInstructions();
    auto pos = std::find(list.begin(), list.end(), anchor);
    inst->setParent(bb);
    list.insert(pos, inst);
}

static void projectFill(const FillInfo& fill) {
    BasicBlock* outerBB = fill.inner->getParent();
    auto innerPos = std::find(outerBB->getInstructions().begin(),
                              outerBB->getInstructions().end(), fill.inner);

    auto* base = new GetElementPtrInst(fill.global, new ConstantInt(0), nullptr);
    base->setName("rp.base");
    auto* rowIndex = new LoadInst(fill.outer->getIVAddr(), nullptr);
    rowIndex->setName("rp.row.index");
    auto* row = new GetElementPtrInst(base, rowIndex, nullptr);
    row->setName("rp.row");
    auto* cell = new GetElementPtrInst(row, new ConstantInt(0), nullptr);
    cell->setName("rp.cell");
    auto* clear = new StoreInst(new ConstantInt(0), cell, nullptr);
    std::vector<Instruction*> setup = {base, rowIndex, row, cell, clear};
    for (Instruction* inst : setup) {
        inst->setParent(outerBB);
        outerBB->getInstructions().insert(innerPos, inst);
    }

    auto* old = new LoadInst(cell, nullptr);
    old->setName("rp.old");
    auto* sum = new BinaryInst(Instruction::Add, old, fill.input, nullptr);
    sum->setName("rp.sum");
    auto* publish = new StoreInst(sum, cell, nullptr);
    insertBefore(fill.store, old);
    insertBefore(fill.store, sum);
    insertBefore(fill.store, publish);
    IRRewriter::eraseOp(fill.store);
}

static bool onlyExpectedGlobalUses(GlobalVariable* global,
                                   const std::set<Instruction*>& accepted) {
    std::vector<Value*> work = {global};
    std::set<Value*> seen = {global};
    while (!work.empty()) {
        Value* value = work.back();
        work.pop_back();
        for (auto* user : value->getUsers()) {
            auto* inst = dyn_cast<Instruction>(user);
            if (!inst) return false;
            if (isa<GetElementPtrInst>(inst)) {
                if (seen.insert(inst).second) work.push_back(inst);
                continue;
            }
            if (!accepted.count(inst)) return false;
        }
    }
    return true;
}

} // namespace

bool ReductionProjection::run() {
    for (auto* function : M->getFunctions()) {
        KernelInfo kernel;
        if (!matchKernel(function, kernel)) continue;

        std::vector<CallInst*> calls;
        std::set<GlobalVariable*> projectedGlobals;
        std::vector<Value*> callExtents;
        bool callsOK = true;
        for (auto* caller : M->getFunctions()) {
            std::vector<Instruction*> all;
            collectRegion(caller->getBody(), all);
            for (auto* inst : all) {
                auto* call = dyn_cast<CallInst>(inst);
                if (!call || call->getFunction() != function) continue;
                if (call->getNumOperands() != 5) { callsOK = false; break; }
                auto* source = globalRoot(call->getOperand(3));
                auto* destination = globalRoot(call->getOperand(4));
                auto* coefficientGlobal = globalRoot(call->getOperand(2));
                if (!source || !destination || source == destination ||
                    !coefficientGlobal || coefficientGlobal == source ||
                    coefficientGlobal == destination) {
                    callsOK = false;
                    break;
                }
                projectedGlobals.insert(source);
                projectedGlobals.insert(destination);
                callExtents.push_back(call->getOperand(1));
                calls.push_back(call);
            }
            if (!callsOK) break;
        }
        if (!callsOK || calls.empty() || projectedGlobals.size() != 2) continue;

        ReductionInfo reduction;
        FillInfo fill;
        bool foundReduction = false;
        bool foundFill = false;
        for (auto* caller : M->getFunctions()) {
            std::vector<Instruction*> all;
            collectRegion(caller->getBody(), all);
            for (auto* inst : all) {
                auto* loop = dyn_cast<ForInst>(inst);
                if (!loop) continue;
                ReductionInfo candidateReduction;
                if (matchReductionLoop(loop, candidateReduction) &&
                    projectedGlobals.count(candidateReduction.global)) {
                    if (foundReduction) { foundReduction = false; goto reject; }
                    reduction = candidateReduction;
                    foundReduction = true;
                }
            }
        }
        if (!foundReduction) continue;

        for (auto* caller : M->getFunctions()) {
            std::vector<Instruction*> all;
            collectRegion(caller->getBody(), all);
            for (auto* inst : all) {
                auto* loop = dyn_cast<ForInst>(inst);
                if (!loop) continue;
                FillInfo candidateFill;
                if (matchFillLoop(loop, reduction.global, candidateFill)) {
                    if (foundFill) { foundFill = false; goto reject; }
                    fill = candidateFill;
                    foundFill = true;
                }
            }
        }
        if (!foundFill) continue;

        // All projected loops and calls must cover the same dynamic extent in
        // the caller.  Loads from the same scalar slot compare via sameIndex.
        if (!sameIndex(fill.inner->getStop(), reduction.inner->getStop()))
            continue;
        for (Value* extent : callExtents)
            if (!sameIndex(extent, fill.inner->getStop())) goto reject;

        // At module scope, projected globals may only be materialized as call
        // arguments, filled from input, or observed by the final reduction.
        {
            std::set<Instruction*> accepted(calls.begin(), calls.end());
            accepted.insert(fill.store);
            std::vector<Instruction*> reductionInsts;
            collectRegion(reduction.inner->getBodyRegion(), reductionInsts);
            accepted.insert(reductionInsts.begin(), reductionInsts.end());
            for (auto* global : projectedGlobals)
                if (!onlyExpectedGlobalUses(global, accepted)) goto reject;
        }

        projectFill(fill);
        kernel.clearProjectedLoop->setOperand(1, new ConstantInt(1));
        kernel.updateProjectedLoop->setOperand(1, new ConstantInt(1));
        reduction.inner->setOperand(1, new ConstantInt(1));
        return true;

    reject:
        continue;
    }
    return false;
}
