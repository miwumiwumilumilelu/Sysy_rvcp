#ifndef MCINVARIANTHOIST_H
#define MCINVARIANTHOIST_H

#include "MCFunction.h"

namespace sysy {
namespace rv {

class MCInvariantHoistPass {
    static bool hoistInvariant(MCFunction* func);
public:
    void run(MCFunction* func);
};

}
}

#endif
