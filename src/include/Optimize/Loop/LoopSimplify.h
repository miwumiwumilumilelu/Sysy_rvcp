#ifndef LOOPSIMPLIFY_H
#define LOOPSIMPLIFY_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/LoopInfo.h"

namespace sysy {

class LoopSimplify {
    Module* M;

    static BasicBlock* mergeLatches(Loop* L);
    static bool dedicateExits(Loop* L, Dominators& dt);
    static bool runOnFunction(Function* f);

public:
    explicit LoopSimplify(Module* m) : M(m) {}
    bool run();
};

}

#endif
