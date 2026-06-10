#ifndef WHILETOFOR_H
#define WHILETOFOR_H

#include "../../IR/Module.h"

namespace sysy {

// 1. bodyBB The last line writes ivAddr, store(load(iv)±step, iv).
// 2. bodyRegion are no other ivAddr write from unknown sources.
// 3. bound is not modified in the body.
// 4. store(iv, startVal) can be found in the parent block before while.
class WhileToFor {
    Module* M;

    bool runFunc(Function* f);
    bool runRegion(Region* r);
    bool whileImpl(WhileInst* wi);

public:
    explicit WhileToFor(Module* m) : M(m) {}
    bool run();
};

}

#endif
