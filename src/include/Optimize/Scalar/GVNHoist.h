#ifndef GVNHOIST_H
#define GVNHOIST_H

#include "IR/Module.h"

namespace sysy {

// Branches to their nearest common dominator (LCA).
// Handles cases LICM misses: same expression in then/else branches,
// or across different paths that converge at a merge block.
class GVNHoist {
public:
    GVNHoist(Module* m) : M(m) {}
    bool run();

private:
    Module* M;
    bool runFunc(Function* f);
};

}
#endif
