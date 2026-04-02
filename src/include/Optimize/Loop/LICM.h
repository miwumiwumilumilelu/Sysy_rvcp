#ifndef LICM_H
#define LICM_H

#include "IR/Module.h"
#include "Optimize/Analysis/LoopInfo.h"
#include "Optimize/Analysis/SCEV.h"
#include <unordered_map>

namespace sysy {

class LICM {
public:
    LICM(Module* m) : M(m) {}
    bool run();

private:
    Module* M;
    std::unordered_map<Function*, bool> purityCache;
    std::unordered_map<Function*, bool> readOnlyCache;

    bool runFunc(Function* f);
    bool hoistLoop(Loop* L, Dominators& dt, SCEV& scev);
    // Promote loop-invariant load/store pairs to registers.
    // Inserts preload before loop, loop phi at header, store at exit.
    bool promoteLoop(Loop* L, Dominators& dt);
    bool tryHoistSubloop(Loop* outer);
    bool isFullyOuterInvariant(Loop* outer, Loop* inner);
    bool unifyIndVars(Loop* L, SCEV& scev);
};

}
#endif
