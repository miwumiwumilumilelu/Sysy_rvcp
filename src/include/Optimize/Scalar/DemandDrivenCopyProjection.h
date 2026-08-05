#ifndef DEMANDDRIVENCOPYPROJECTION_H
#define DEMANDDRIVENCOPYPROJECTION_H

#include "../../IR/Module.h"

namespace sysy {

class DemandDrivenCopyProjection {
    Module* M;
public:
    explicit DemandDrivenCopyProjection(Module* module) : M(module) {}
    bool run();
};

}
#endif
