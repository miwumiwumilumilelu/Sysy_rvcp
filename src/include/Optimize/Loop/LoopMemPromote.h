#ifndef LOOPMEMPROMOTE_H
#define LOOPMEMPROMOTE_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/SCEV.h"
#include <unordered_map>

namespace sysy {

class LoopMemPromote {
public:
    explicit LoopMemPromote(Module* m) : M(m) {}

    bool run();
    bool runOnLoop(Loop* L, Dominators& dt, SCEV& scev);

private:
    Module* M;
    std::unordered_map<Function*, bool> purityCache;

    bool runFunc(Function* f);
    bool promoteLoop(Loop* L, Dominators& dt);
};

}
#endif
