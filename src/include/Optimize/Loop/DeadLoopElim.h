#ifndef DEADLOOPELIM_H
#define DEADLOOPELIM_H

#include "IR/Module.h"

namespace sysy {

class DeadLoopElim {
    Module* M;

public:
    explicit DeadLoopElim(Module* m) : M(m) {}
    bool run();
};

}
#endif
