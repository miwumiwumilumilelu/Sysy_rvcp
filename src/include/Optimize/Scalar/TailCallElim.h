#ifndef TAILCALLELIM_H
#define TAILCALLELIM_H

#include "../../IR/Module.h"

namespace sysy {

class TCE {
private:
    Module* M;
    bool runFunc(Function* f);

public:
    explicit TCE(Module* m) : M(m) {}
    bool run();
};

}

#endif 