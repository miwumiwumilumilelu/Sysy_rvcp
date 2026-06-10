#ifndef LOOPGVN_H
#define LOOPGVN_H

#include "../../IR/Module.h"

namespace sysy {

class LoopGVN {
public:
    explicit LoopGVN(Module* m) : M(m) {}
    bool run();

private:
    Module* M;
    bool runFunc(Function* f);
};

}

#endif
