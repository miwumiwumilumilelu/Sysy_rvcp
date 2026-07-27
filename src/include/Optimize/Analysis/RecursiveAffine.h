#ifndef RECURSIVEAFFINE_H
#define RECURSIVEAFFINE_H

#include "../../IR/Module.h"
#include <vector>

namespace sysy {

// A i32-based affine domain.  
// It is used to prove that one recursive argument is an accumulator.
// coefficient * accumulator + bias
struct AffineValue {
    uint32_t coefficient = 0;
    uint32_t bias = 0;
};

// 
struct RecursiveAffineSummary {
    // Indicates which parameter can be excluded from the cache key in memo opt.
    unsigned accumulatorIndex = 0;
    // There are frequent key accesses during a cache hit, 
    // so booleanCoefficient is used to reduce memory access.
    // When booleanCoefficient is true, That is A is either 0 or 1，
    // then tag = A + 1, Cache key turns to:
    // {tag, B}
    // tag = 0  -> invalid, no cache
    // tag = 1  -> valid，A=0
    // tag = 2  -> valid，A=1
    bool booleanCoefficient = false;
    struct Transition {
        CallInst* call = nullptr;
        uint32_t coefficient = 0;
    };
    // Corresponds to the return upon non-recursive termination.
    struct Terminal {
        ReturnInst* ret = nullptr;
        uint32_t coefficient = 0;
    };
    std::vector<Transition> transitions;
    std::vector<Terminal> terminals;
};

// Recognize strict tail-recursive functions whose result is affine in one
// argument while control flow, memory addresses, and all other recursive state
// are independent of that argument.
class RecursiveAffineAnalysis {
    Function* F;
    bool analyzeArgument(unsigned index, RecursiveAffineSummary& summary);
public:
    explicit RecursiveAffineAnalysis(Function* f) : F(f) {}

    bool run(RecursiveAffineSummary& summary);   
};

}
#endif
