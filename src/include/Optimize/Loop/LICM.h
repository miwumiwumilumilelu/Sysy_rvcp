#ifndef LICM_H
#define LICM_H

#include "../../IR/Module.h"
#include "../Analysis/Dominators.h"
#include "../Analysis/SCEV.h"
#include <unordered_map>

namespace sysy {

class LICM {
public:
    explicit LICM(Module* m) : M(m) {}

    bool run();
    bool runOnLoop(Loop* L, Dominators& dt, SCEV& scev);

private:
    Module* M;
    std::unordered_map<Function*, bool> purityCache;
    std::unordered_map<Function*, bool> readOnlyCache;

    bool runFunc(Function* f);
    bool hoistLoop(Loop* L, Dominators& dt, SCEV& scev);
};

}
#endif
