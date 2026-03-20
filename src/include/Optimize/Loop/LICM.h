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
    bool rotateLoop(Loop* L, Function* f);
    bool hoistLoop(Loop* L, Dominators& dt, SCEV& scev);
    // Hoist an entire outer-invariant inner loop before the outer loop.
    bool tryHoistSubloop(Loop* outer);
    // Defined in a block that belongs to outer but not to inner.
    bool isFullyOuterInvariant(Loop* outer, Loop* inner);
};

}
#endif
