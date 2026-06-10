#ifndef SUBLOOPHOIST_H
#define SUBLOOPHOIST_H

#include "../../IR/Module.h"
#include "../Analysis/LoopInfo.h"
#include "../Analysis/Dominators.h"
#include <unordered_map>

namespace sysy {

class SubloopHoist {
public:
    SubloopHoist(Module* m) : M(m) {}
    bool run();

private:
    Module* M;
    std::unordered_map<Function*, bool> purityCache;

    bool runFunc(Function* f);
    bool tryHoistSubloop(Loop* outer);
    bool isFullyOuterInvariant(Loop* outer, Loop* inner);
};

}
#endif
