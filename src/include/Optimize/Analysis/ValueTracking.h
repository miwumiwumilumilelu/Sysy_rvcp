#ifndef VALUETRACKING_H
#define VALUETRACKING_H

#include "KnownBits.h"
#include "NonNeg.h"
#include "Range.h"

namespace sysy {

// LLVM-style value query context.
// vt.range(x)
// vt.isNonNeg(x)
// vt.knownBits(x)
// vt.knownBool(cond, out)
class ValueTracking {
    Function* F;
    std::unique_ptr<RangeAnalysis> RA;
    RangeAnalysis& ra() {
        if (!RA)
            RA = std::make_unique<RangeAnalysis>(F);
        return *RA;
    }

    std::unique_ptr<NonNegAnalysis> NN;
    NonNegAnalysis& nn() {
        if (!NN)
            NN = std::make_unique<NonNegAnalysis>(F, &ra());
        return *NN;
    }

    std::unique_ptr<KnownBitsAnalysis> KB;
    KnownBitsAnalysis& kb() {
        if (!KB)
            KB = std::make_unique<KnownBitsAnalysis>(&nn());
        return *KB;
    }

public:
    explicit ValueTracking(Function* f) : F(f) {}

    bool has(Value* v) { return ra().has(v); }
    IRange range(Value* v) { return ra().get(v); }

    bool isNonNeg(Value* v) { return nn().isNonNeg(v); }
    bool isPos(Value* v) {
        return ra().isPositive(v);
    }

    KBits knownBits(Value* v) { return kb().get(v); }
    bool knownBool(Value* v, bool& out) {
        if (auto* ci = dyn_cast<ConstantInt>(v)) {
            out = ci->getValue() != 0;
            return true;
        }

        auto* cmp = dyn_cast<ICmpInst>(v);
        if (!cmp) return false;
        Value* lhs = cmp->getOperand(0);
        Value* rhs = cmp->getOperand(1);
        if (!has(lhs) || !has(rhs)) return false;

        IRange L = range(lhs);
        IRange R = range(rhs);
        switch (cmp->getPredicate()) {
            case ICmpInst::EQ:
                if (L.low == L.high && R.low == R.high) {
                    out = L.low == R.low;
                    return true;
                }
                if (L.high < R.low || R.high < L.low) {
                    out = false;
                    return true;
                }
                return false;
            case ICmpInst::NE:
                if (L.low == L.high && R.low == R.high) {
                    out = L.low != R.low;
                    return true;
                }
                if (L.high < R.low || R.high < L.low) {
                    out = true;
                    return true;
                }
                return false;
            case ICmpInst::SGT:
                if (L.low > R.high) {
                    out = true;
                    return true;
                }
                if (L.high <= R.low) {
                    out = false;
                    return true;
                }
                return false;
            case ICmpInst::SGE:
                if (L.low >= R.high) {
                    out = true;
                    return true;
                }
                if (L.high < R.low) {
                    out = false;
                    return true;
                }
                return false;
            case ICmpInst::SLT:
                if (L.high < R.low) {
                    out = true;
                    return true;
                }
                if (L.low >= R.high) {
                    out = false;
                    return true;
                }
                return false;
            case ICmpInst::SLE:
                if (L.high <= R.low) {
                    out = true;
                    return true;
                }
                if (L.low > R.high) {
                    out = false;
                    return true;
                }
                return false;
        }
        return false;
    }
};

}

#endif
