#ifndef LoopExitFold_H
#define LoopExitFold_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/SCEV.h"

namespace sysy {

class LoopExitFold {
public:
    LoopExitFold(Module* m) : M(m) {}
    bool run();
    bool runOnLoop(Loop* L, Dominators& dt, SCEV& scev);

private:
    Module* M;

    bool runFunc(Function* f);
    bool deduplicateRec(Loop* L, SCEV& scev);
    bool foldExitValues(Loop* L, SCEV& scev);
};

}
#endif
