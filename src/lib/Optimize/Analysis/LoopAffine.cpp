#include "../../../include/Optimize/Analysis/LoopAffine.h"
#include "../../../include/IR/Instruction.h"

using namespace sysy;

namespace {

static Loop* containingSelectedLoop(Instruction* inst,
                                    const std::set<Loop*>& loops) {
    if (!inst || !inst->getParent()) return nullptr;
    for (auto* loop : loops)
        if (loop && loop->has(inst->getParent())) return loop;
    return nullptr;
}

} // namespace

bool LoopAffineAnalysis::isInvariant(Value* value) const {
    auto* inst = dyn_cast<Instruction>(value);
    if (!inst) return true;
    return containingSelectedLoop(inst, Loops) == nullptr;
}

PhiInst* LoopAffineAnalysis::induction(Value* value) const {
    auto* phi = dyn_cast<PhiInst>(value);
    if (!phi) return nullptr;
    for (auto* loop : Loops)
        if (loop && phi->getParent() == loop->head) return phi;
    return nullptr;
}

LoopAffineExpr LoopAffineAnalysis::add(const LoopAffineExpr& lhs,
                                       const LoopAffineExpr& rhs,
                                       int rhsSign) {
    if (!lhs.valid || !rhs.valid) return {};
    LoopAffineExpr result = lhs;
    result.valid = true;
    result.constant += rhsSign * rhs.constant;

    for (auto& [term, coefficient] : rhs.invariantTerms)
        result.invariantTerms[term] += rhsSign * coefficient;
    for (auto& [iv, coefficient] : rhs.constantCoefficients)
        result.constantCoefficients[iv] += rhsSign * coefficient;
    for (auto& [iv, coefficient] : rhs.symbolicCoefficients) {
        auto found = result.symbolicCoefficients.find(iv);
        if (found == result.symbolicCoefficients.end() && rhsSign == 1)
            result.symbolicCoefficients[iv] = coefficient;
        else
            return {}; // symbolic addition/subtraction needs an expression node
    }
    return result;
}

LoopAffineExpr LoopAffineAnalysis::scale(const LoopAffineExpr& expr,
                                         int64_t factor) {
    if (!expr.valid) return {};
    LoopAffineExpr result = expr;
    result.constant *= factor;
    for (auto& [term, coefficient] : result.invariantTerms)
        coefficient *= factor;
    for (auto& [iv, coefficient] : result.constantCoefficients)
        coefficient *= factor;
    if (factor != 1 && !result.symbolicCoefficients.empty()) return {};
    return result;
}

LoopAffineExpr LoopAffineAnalysis::analyzeImpl(Value* value) {
    if (auto* c = dyn_cast<ConstantInt>(value)) {
        LoopAffineExpr result;
        result.valid = true;
        result.constant = c->getValue();
        return result;
    }
    if (auto* iv = induction(value)) {
        LoopAffineExpr result;
        result.valid = true;
        result.constantCoefficients[iv] = 1;
        return result;
    }
    if (isInvariant(value)) {
        LoopAffineExpr result;
        result.valid = true;
        result.invariantTerms[value] = 1;
        return result;
    }
    auto* bin = dyn_cast<BinaryInst>(value);
    if (!bin) return {};
    if (bin->getOpID() == Instruction::Add ||
        bin->getOpID() == Instruction::Sub) {
        auto lhs = analyze(bin->getOperand(0));
        auto rhs = analyze(bin->getOperand(1));
        return add(lhs, rhs, bin->getOpID() == Instruction::Add ? 1 : -1);
    }
    if (bin->getOpID() != Instruction::Mul) return {};

    Value* lhs = bin->getOperand(0);
    Value* rhs = bin->getOperand(1);
    if (auto* c = dyn_cast<ConstantInt>(rhs))
        return scale(analyze(lhs), c->getValue());
    if (auto* c = dyn_cast<ConstantInt>(lhs))
        return scale(analyze(rhs), c->getValue());

    // Preserve IV * runtime-invariant-stride as a symbolic coefficient.
    PhiInst* iv = induction(lhs);
    Value* coefficient = rhs;
    if (!iv) {
        iv = induction(rhs);
        coefficient = lhs;
    }
    if (!iv || !isInvariant(coefficient)) return {};
    LoopAffineExpr result;
    result.valid = true;
    result.symbolicCoefficients[iv] = coefficient;
    return result;
}

LoopAffineExpr LoopAffineAnalysis::analyze(Value* value) {
    auto found = Cache.find(value);
    if (found != Cache.end()) return found->second;
    if (!value || !Active.insert(value).second) return {};
    LoopAffineExpr result = analyzeImpl(value);
    Active.erase(value);
    Cache[value] = result;
    return result;
}
