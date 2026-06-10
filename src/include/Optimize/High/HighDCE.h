#ifndef HIGHDCE_H
#define HIGHDCE_H

#include "../../IR/Module.h"

namespace sysy {

class HighDCE {
    Module* M;
    static bool processFunc(Function* f);
public:
    explicit HighDCE(Module* m) : M(m) {}
    bool run();
};

}

#endif
