#ifndef LoopStrengthReduce_H
#define LoopStrengthReduce_H

#include "../../IR/Module.h"
#include "../Analysis/Dominators.h"
#include "../Analysis/SCEV.h"

namespace sysy {

// Rewrite repeated MUL and GEP in the loop to Phi recursion.
class LoopStrengthReduce {
public:
    explicit LoopStrengthReduce(Module* m) : M(m) {}
    bool run();

private:
    Module* M;
    bool runFunc(Function* f);
    bool runOnLoop(Loop* L, SCEV& scev);
};

}
#endif
