#ifndef LCSSA_H
#define LCSSA_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/LoopInfo.h"
#include <map>
#include <set>

namespace sysy {

// All values flowing from the loop to the outside of the loop
// are unified through the phi that goes out of the loop.
class LCSSA {
    Module* M;

    static Value* findValue(BasicBlock* bb,
                            std::map<BasicBlock*, Value*>& reachMap,
                            Dominators& dt,
                            const std::set<BasicBlock*>& loopBBs);

    // Insert LCSSA phis for all loop-escaping defs in L (post-order).
    static bool processLoop(Loop* L, Dominators& dt);
    static bool runOnFunction(Function* f);

public:
    explicit LCSSA(Module* m) : M(m) {}
    bool run();
};

}

#endif
