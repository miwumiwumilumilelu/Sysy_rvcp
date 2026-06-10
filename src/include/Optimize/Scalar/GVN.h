#ifndef GVN_H
#define GVN_H

#include "../../IR/Module.h"
#include <unordered_map>

namespace sysy {

// Global Value Numbering: domtree-scoped CSE across basic blocks.
// Eliminates cross-block redundancies that local CSE misses.
class GVN {
public:
    GVN(Module* m) : M(m) {}
    bool run();

private:
    Module* M;
    std::unordered_map<Function*, bool> purityCache;
    bool runFunc(Function* f);
};

}
#endif
