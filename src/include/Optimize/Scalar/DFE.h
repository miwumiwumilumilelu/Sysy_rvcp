#ifndef DFE_H
#define DFE_H

#include "../../IR/Module.h"

namespace sysy {

class DFE {
public:
    explicit DFE(Module* m) : M(m) {}
    bool run();

private:
    Module* M;
};

}

#endif
