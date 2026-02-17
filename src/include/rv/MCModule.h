#ifndef MCMODULE_H
#define MCMODULE_H

#include "rv/MCFunction.h"
#include <vector>

namespace sysy {

class MCModule {
public:
    std::vector<MCFunc*> funcs;
    // TODO: Contains the definition of Global Variables.

    void add(MCFunc* f) {
        f->parent = this;
        funcs.push_back(f);
    }
};

}

#endif