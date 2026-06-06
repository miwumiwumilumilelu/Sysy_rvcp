#include "Optimize/Analysis/KnownBits.h"
#include "Optimize/Analysis/NonNeg.h"
#include "IR/Instruction.h"

using namespace sysy;

KBits KnownBitsAnalysis::seed(Value* v) const {
    KBits bits;
    if (NonNeg && NonNeg->isNonNeg(v))
        bits.zeros |= (1u << 31);
    return bits;
}

KBits KnownBitsAnalysis::get(Value* v) {
    if (!v) return {};

    auto it = Cache.find(v);
    if (it != Cache.end()) return it->second;

    KBits seedBits = seed(v);
    // Reserve a seeded slot before recursing,
    // so cyclic phis can still expose proven facts,
    // such as a known-zero sign bit to their loop-carried users.
    //
    // %crc = phi [0], [%next]
    // %next = (%crc >> 8) ^ table
    //
    // When %crc is encountered again, the seed is returned directly.
    Cache[v] = seedBits;

    KBits result = seedBits;
    if (auto* ci = dyn_cast<ConstantInt>(v)) {
        uint32_t val = (uint32_t)ci->getValue();
        result = {~val, val};
    } else if (auto* inst = dyn_cast<BinaryInst>(v)) {
        KBits L = get(inst->getOperand(0));
        KBits R = get(inst->getOperand(1));
        switch (inst->getOpID()) {
            case Instruction::And:
                result = {L.zeros | R.zeros, L.ones & R.ones};
                break;
            case Instruction::Or:
                result = {L.zeros & R.zeros, L.ones | R.ones};
                break;
            case Instruction::Xor:
                result = {
                    (L.zeros & R.zeros) | (L.ones & R.ones),
                    (L.zeros & R.ones) | (L.ones & R.zeros)
                };
                break;
            case Instruction::Shl: {
                auto* sh = dyn_cast<ConstantInt>(inst->getOperand(1));
                if (sh && sh->getValue() >= 0 && sh->getValue() < 32) {
                    int k = sh->getValue();
                    // zeros: Pad the lower k bits with 0 via (1u << k) - 1u.
                    result = {(L.zeros << k) | ((1u << k) - 1u), L.ones << k};
                }
                break;
            }
            case Instruction::Ashr: {
                auto* sh = dyn_cast<ConstantInt>(inst->getOperand(1));
                if (sh && sh->getValue() >= 0 && sh->getValue() < 32) {
                    int k = sh->getValue();
                    result = {L.zeros >> k, L.ones >> k};
                    // Pad with sign bit.
                    if (k > 0) {
                        // mask
                        // if k = 8 -> high = 0xff000000
                        uint32_t high = ~0u << (32 - k);
                        if (L.zeros & (1u << 31))
                            result.zeros |= high;
                        if (L.ones & (1u << 31))
                            result.ones |= high;
                    }
                }
                break;
            }
            default:
                break;
        }
    } else if (auto* phi = dyn_cast<PhiInst>(v)) {
        int n = phi->getNumOperands();
        if (n > 0) {
            uint32_t zeros = ~0u;
            uint32_t ones = ~0u;
            // Only the known bits that are common to all incoming Value will remain.
            for (int i = 0; i < n; i += 2) {
                KBits opBits = get(phi->getOperand(i));
                zeros &= opBits.zeros;
                ones &= opBits.ones;
            }
            result = {zeros, ones};
        }
    } else if (auto* sel = dyn_cast<SelectInst>(v)) {
        // select(c, x, y): a bit is known zero only if zero in both branches,
        // and known one only if one in both branches.
        KBits T = get(sel->getTrueVal());
        KBits F = get(sel->getFalseVal());
        result = {T.zeros & F.zeros, T.ones & F.ones};
    } else if (isa<ICmpInst>(v) || isa<FCmpInst>(v)) {
        // 0 or 1.
        // ~1u = 0xfffffffe
        // bit1 to bit31 is 0, but bit0 is unknown.
        result = {~1u, 0u};
    }

    result.zeros |= seedBits.zeros;
    // zeros & ones == 0
    // So remove all bits that are already known to be zero from ones.
    result.ones &= ~result.zeros;
    return Cache[v] = result;
}
