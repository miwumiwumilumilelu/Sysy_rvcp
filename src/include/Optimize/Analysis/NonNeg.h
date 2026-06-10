#ifndef NONNEG_H
#define NONNEG_H

#include "../../IR/Module.h"
#include <set>

namespace sysy {

class RangeAnalysis;

// Proves integer values are non-negative with an optimistic fixed point.
// This is useful for cyclic phi.
class NonNegAnalysis {
    Function* F;
    // External RangeAnalysis
    const RangeAnalysis* ExtRA;
    std::unique_ptr<RangeAnalysis> OwnRA;
    // NonNegSet
    std::set<Value*> S; 

    void analyze();
    const RangeAnalysis& ranges();
public:
    explicit NonNegAnalysis(Function* f, const RangeAnalysis* RA = nullptr);

    bool isNonNeg(Value* v) const;
};

}

#endif
