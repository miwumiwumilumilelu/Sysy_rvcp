#ifndef SSAINLINE_H
#define SSAINLINE_H

#include "IR/Module.h"

namespace sysy {

class SSAInline {
    Module* M;
    int threshold;

public:
    static bool isRecursive(Function* f);
    static int countInsts(Function* f);
    explicit SSAInline(Module* m, int threshold = 200) : M(m), threshold(threshold) {}
    bool run();

private:
    bool isInlineable(CallInst* call, bool callSiteInLoop) const;

    void doInline(CallInst* call);

    // Move AllocaInst to entry block for better readability.
    static void AllocaHoist(Function* func);
};

}

#endif
