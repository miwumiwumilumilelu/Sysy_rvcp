#ifndef LOOPAFFINE_H
#define LOOPAFFINE_H

#include "LoopInfo.h"
#include <map>
#include <set>

namespace sysy {

// An affine expression relative to a selected loop nest.  Coefficients may be
// runtime values, but must be invariant with respect to every selected loop.
//
//   base + sum(coefficient[iv] * iv)
//
// The domain deliberately preserves symbolic products such as i * stride
// when stride is loop invariant; ordinary SCEV only represents constant
// multipliers.
struct LoopAffineExpr {
    int64_t constant = 0;
    std::map<Value*, int64_t> invariantTerms;
    std::map<PhiInst*, Value*> symbolicCoefficients;
    std::map<PhiInst*, int64_t> constantCoefficients;

    bool valid = false;
    bool isInvariant() const {
        return valid && symbolicCoefficients.empty() &&
               constantCoefficients.empty();
    }
};

class LoopAffineAnalysis {
    const std::set<Loop*>& Loops;
    std::map<Value*, LoopAffineExpr> Cache;
    std::set<Value*> Active;

    bool isInvariant(Value* value) const;
    PhiInst* induction(Value* value) const;
    LoopAffineExpr analyzeImpl(Value* value);
    LoopAffineExpr add(const LoopAffineExpr& lhs,
                       const LoopAffineExpr& rhs, int rhsSign);
    LoopAffineExpr scale(const LoopAffineExpr& expr, int64_t factor);

public:
    explicit LoopAffineAnalysis(const std::set<Loop*>& loops) : Loops(loops) {}
    LoopAffineExpr analyze(Value* value);
};

}

#endif
