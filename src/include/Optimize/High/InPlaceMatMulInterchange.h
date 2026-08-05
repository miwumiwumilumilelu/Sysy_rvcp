#ifndef INPLACEMATMULINTERCHANGE_H
#define INPLACEMATMULINTERCHANGE_H

#include "../../IR/Module.h"

namespace sysy {

// Interchange an in-place integer matrix product after isolating the diagonal
// term that would otherwise violate the interchange dependence.
class InPlaceMatMulInterchange {
public:
    explicit InPlaceMatMulInterchange(Module* module) : M(module) {}
    bool run();

private:
    Module* M;
};

}

#endif
