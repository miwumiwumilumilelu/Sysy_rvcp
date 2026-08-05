#include "../../../include/Optimize/Analysis/AffineCopySummary.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "../../../include/IR/Instruction.h"
#include <set>

using namespace sysy;

namespace {

struct IndexedAddress {
    Value* base = nullptr;
    Value* index = nullptr;
};

static IndexedAddress decomposeOneDimensional(Value* pointer) {
    auto* gep = dyn_cast<GetElementPtrInst>(pointer);
    if (!gep) return {};
    Value* base = gep->getOperand(0);
    Value* index = gep->getOperand(1);
    // Ignore a canonical array-to-pointer decay at index zero.
    if (auto* decay = dyn_cast<GetElementPtrInst>(base)) {
        auto* zero = dyn_cast<ConstantInt>(decay->getOperand(1));
        if (!zero || zero->getValue() != 0) return {};
        base = decay->getOperand(0);
    }
    return {base, index};
}

static PhiInst* inductionOf(Loop* loop) {
    PhiInst* result = nullptr;
    for (auto* inst : loop->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        bool recurrence = false;
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            auto* from = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
            auto* next = dyn_cast<BinaryInst>(phi->getOperand(i));
            if (!from || !loop->has(from) || !next) continue;
            if ((next->getOpID() == Instruction::Add ||
                 next->getOpID() == Instruction::Sub) &&
                next->getOperand(0) == phi &&
                isa<ConstantInt>(next->getOperand(1)))
                recurrence = true;
        }
        if (!recurrence) continue;
        if (result) return nullptr;
        result = phi;
    }
    return result;
}

static bool zeroBasedUnitLoop(Loop* loop, PhiInst* iv, SCEV& scev,
                              Value*& bound) {
    ExitBranchInfo exit;
    if (!analyzeExitBranch(loop, loop->head, scev, exit) ||
        exit.continuePred != ICmpInst::SLT || exit.lhs != iv)
        return false;
    bound = exit.rhs;
    bool zero = false, unit = false;
    for (int k = 0; k < iv->getNumOperands(); k += 2) {
        auto* from = dyn_cast<BasicBlock>(iv->getOperand(k + 1));
        if (!from) return false;
        if (!loop->has(from)) {
            auto* c = dyn_cast<ConstantInt>(iv->getOperand(k));
            zero |= c && c->getValue() == 0;
        } else if (auto* add = dyn_cast<BinaryInst>(iv->getOperand(k))) {
            auto* c = dyn_cast<ConstantInt>(add->getOperand(1));
            unit |= add->getOpID() == Instruction::Add &&
                    add->getOperand(0) == iv && c && c->getValue() == 1;
        }
    }
    return zero && unit;
}

// Recognize min(limit, outer + 1) semantically.  This is the triangular
// iteration domain 0 <= inner <= outer, additionally clipped by limit.
static bool triangularBound(Value* value, Value* limit, PhiInst* outer) {
    auto* sel = dyn_cast<SelectInst>(value);
    if (!sel) return false;
    auto* cmp = dyn_cast<ICmpInst>(sel->getOperand(0));
    auto* next = dyn_cast<BinaryInst>(sel->getOperand(2));
    auto* one = next ? dyn_cast<ConstantInt>(next->getOperand(1)) : nullptr;
    return cmp && cmp->getPredicate() == ICmpInst::SLT &&
           cmp->getOperand(0) == limit && cmp->getOperand(1) == next &&
           sel->getOperand(1) == limit && next &&
           next->getOpID() == Instruction::Add &&
           next->getOperand(0) == outer && one && one->getValue() == 1;
}

} // namespace

bool AffineCopyAnalysis::run(AffineCopySummary& summary) {
    if (!F || F->getBody()->getBlocks().empty()) return false;
    Dominators dominators(F);
    dominators.run();
    LoopInfo loops(F, dominators);

    StoreInst* copyStore = nullptr;
    LoadInst* copyLoad = nullptr;
    for (auto* bb : F->getBody()->getBlocks())
        for (auto* inst : bb->getInstructions()) {
            if (auto* call = dyn_cast<CallInst>(inst)) return false;
            auto* store = dyn_cast<StoreInst>(inst);
            if (!store) continue;
            auto* load = dyn_cast<LoadInst>(store->getOperand(0));
            if (!load || copyStore) return false;
            copyStore = store;
            copyLoad = load;
        }
    if (!copyStore || !copyLoad) return false;

    Loop* inner = loops.loopOf(copyStore->getParent());
    if (!inner || !inner->up || !inner->sub.empty()) return false;
    Loop* outer = inner->up;
    if (outer->sub.size() != 1 || outer->sub.front() != inner) return false;

    // Every memory access in the kernel must be exactly the source load or the
    // destination store.  This excludes hidden state and aliasing side effects.
    for (auto* bb : outer->blocks)
        for (auto* inst : bb->getInstructions()) {
            if (isa<LoadInst>(inst) && inst != copyLoad) return false;
            if (isa<StoreInst>(inst) && inst != copyStore) return false;
            if (isa<CallInst>(inst) || isa<ReturnInst>(inst)) return false;
        }

    IndexedAddress source = decomposeOneDimensional(copyLoad->getOperand(0));
    IndexedAddress destination =
        decomposeOneDimensional(copyStore->getOperand(1));
    if (!source.base || source.base != destination.base) return false;

    PhiInst* outerIV = inductionOf(outer);
    PhiInst* innerIV = inductionOf(inner);
    if (!outerIV || !innerIV || outerIV == innerIV) return false;
    SCEV scev(F, loops);
    Value *outerBound = nullptr, *innerBound = nullptr;
    if (!zeroBasedUnitLoop(outer, outerIV, scev, outerBound) ||
        !zeroBasedUnitLoop(inner, innerIV, scev, innerBound)) return false;
    std::set<Loop*> selected{outer, inner};
    LoopAffineAnalysis affine(selected);
    LoopAffineExpr sourceExpr = affine.analyze(source.index);
    LoopAffineExpr destinationExpr = affine.analyze(destination.index);
    if (!sourceExpr.valid || !destinationExpr.valid) return false;
    auto hasBothIVs = [&](const LoopAffineExpr& expr) {
        auto has = [&](PhiInst* iv) {
            return expr.constantCoefficients.count(iv) ||
                   expr.symbolicCoefficients.count(iv);
        };
        return has(outerIV) && has(innerIV);
    };
    if (!hasBothIVs(sourceExpr) || !hasBothIVs(destinationExpr)) return false;

    // The mixed-radix transform consumes exactly this general triangular
    // domain.  Keep its proof here instead of assuming a source-level if.
    Value* innerLimit = nullptr;
    if (auto* sel = dyn_cast<SelectInst>(innerBound))
        innerLimit = sel->getOperand(1);
    if (!innerLimit || !triangularBound(innerBound, innerLimit, outerIV))
        return false;

    summary = {F, source.base, outerIV, innerIV, outerBound, innerLimit, copyLoad,
               copyStore, std::move(sourceExpr), std::move(destinationExpr)};
    return true;
}
