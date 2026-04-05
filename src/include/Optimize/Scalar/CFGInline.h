#ifndef CFGINLINE_H
#define CFGINLINE_H

#include "Optimize/Analysis/Dominators.h"
#include "IR/Instruction.h"
#include "IR/Module.h"
#include <map>

namespace sysy {

// Flat-CFG inliner before Mem2Reg.
class CFGInline {
    Module* M;
    int threshold;

    static bool hasLoop(Function* f);
    static bool hasPhi(Function* f);
    static bool hasAlloca(Function* f);
    static bool hasNestedCall(Function* f);

    bool isInlineable(Function* f) const;

    void doInline(CallInst* call);
    static void AllocaHoist(Function* func);

public:
    explicit CFGInline(Module* m, int threshold = 200) : M(m), threshold(threshold) {}
    bool run();
};

}

#endif
