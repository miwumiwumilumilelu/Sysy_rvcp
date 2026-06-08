#ifndef STRENGTHREDUCE_H
#define STRENGTHREDUCE_H

#include "IR/Module.h"

namespace sysy {

class ValueTracking;

// Mul(x, 2^k) -> Shl(x, k)
//
// Only perform the following reduction when it can be proven that the operand is non-negative:
// Div(x, 2^k), x>=0 -> Ashr(x, k)
// Mod(x, 2^k), x>=0 -> And(x, 2^k-1)
class StrengthReduce {
private:
    Module* M;

    bool rewriteFunc(Function* f, ValueTracking& vt);

public:
    StrengthReduce(Module* m) : M(m) {}
    bool run();
};

}

#endif
