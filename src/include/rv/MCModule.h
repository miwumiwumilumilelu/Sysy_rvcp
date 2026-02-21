#ifndef MCMODULE_H
#define MCMODULE_H

#include "rv/MCFunction.h"
#include "IR/Module.h"
#include <vector>

namespace sysy {

class MCModule {
public:
    std::vector<MCFunc*> funcs;
    std::vector<GlobalVariable*> globals;

    void add(MCFunc* f) {
        f->parent = this;
        funcs.push_back(f);
    }
};

}

#endif