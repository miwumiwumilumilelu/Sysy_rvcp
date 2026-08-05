#ifndef REDUCTIONPROJECTION_H
#define REDUCTIONPROJECTION_H

#include "../../IR/Module.h"

namespace sysy {

// Project an element-wise integer-linear array dimension through a final
// additive reduction.  The pass is deliberately placed on structured IR,
// where loop dimensions and GEP indices are still explicit.
class ReductionProjection {
public:
    explicit ReductionProjection(Module* module) : M(module) {}

    bool run();

private:
    Module* M;
};

}

#endif
