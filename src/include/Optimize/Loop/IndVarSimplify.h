#ifndef INDVARSIMPLIFY_H
#define INDVARSIMPLIFY_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/SCEV.h"

namespace sysy {

class IndVarSimplify {
public:
    IndVarSimplify(Module* m) : M(m) {}
    bool run();
    bool runOnLoop(Loop* L, Dominators& dt, SCEV& scev);

private:
    Module* M;

    bool runFunc(Function* f);
    bool unifyIndVars(Loop* L, SCEV& scev);
    bool simplifyShiftRec(Loop* L, SCEV& scev);
};

}
#endif
