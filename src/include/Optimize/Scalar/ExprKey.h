#ifndef EXPRKEY_H
#define EXPRKEY_H

#include "../../IR/Module.h"
#include "../../IR/Instruction.h"
#include <cstdint>
#include <tuple>
#include <vector>

namespace sysy {

// Encodes a Value as a hash key. Constants by value, others by pointer.
inline uint64_t vnKey(Value* v) {
    if (!v) return 0;
    if (auto* ci = dyn_cast<ConstantInt>(v))
        return (1ULL << 63) | (uint64_t)(uint32_t)(int64_t)ci->getValue();
    if (isa<ConstantZero>(v))
        return (1ULL << 63);
    if (auto* cf = dyn_cast<ConstantFloat>(v)) {
        uint32_t bits = 0; 
        float f = cf->getValue();
        __builtin_memcpy(&bits, &f, 4);
        return (3ULL << 62) | bits;
    }
    return (uint64_t)(uintptr_t)v;
}

// <opID, lhs_vnKey, rhs_vnKey>
using ExprKey = std::tuple<int, uint64_t, uint64_t>;
// <ptr, arg1, arg2, ...>
using CallKey = std::vector<uint64_t>;

// Return {0,0,0} if inst is impure.
inline ExprKey makeExprKey(Instruction* inst) {
    int sub = 0; 
    bool ok = false;
    if (isa<BinaryInst>(inst) || isa<CastInst>(inst) || isa<GetElementPtrInst>(inst))
        ok = true;
    else if (auto* ic = dyn_cast<ICmpInst>(inst)) { sub = (int)ic->getPredicate(); ok = true; }
    else if (auto* fc = dyn_cast<FCmpInst>(inst)) { sub = (int)fc->getPredicate(); ok = true; }
    if (!ok) return {0, 0, 0};
    Value* a = inst->getNumOperands() > 0 ? inst->getOperand(0) : nullptr;
    Value* b = inst->getNumOperands() > 1 ? inst->getOperand(1) : nullptr;
    uint64_t ka = vnKey(a), kb = vnKey(b);

    // Normalize commutative ops so (a op b) and (b op a) yield the same key.
    auto op = inst->getOpID();
    bool comm = (op == Instruction::Add || op == Instruction::Mul ||
                op == Instruction::FAdd || op == Instruction::FMul);
    if (!comm) {
        if (auto* ic = dyn_cast<ICmpInst>(inst)) {
            auto p = ic->getPredicate();
            comm = (p == ICmpInst::EQ || p == ICmpInst::NE);
        } else if (auto* fc = dyn_cast<FCmpInst>(inst)) {
            auto p = fc->getPredicate();
            comm = (p == FCmpInst::OEQ || p == FCmpInst::ONE);
        }
    }
    if (comm && ka > kb) std::swap(ka, kb);

    return {(int)op * 4399 + sub, ka, kb};
}

}
#endif
