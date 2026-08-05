#ifndef SSAINLINE_H
#define SSAINLINE_H

#include "../../IR/Module.h"

namespace sysy {

struct Loop;
class SCEV;

class SSAInline {
    Module* M;
    int threshold;

public:
    static bool isRecursive(Function* f);
    static int countInsts(Function* f);
    explicit SSAInline(Module* m, int threshold = 200) : M(m), threshold(threshold) {}
    bool run();

    // Inline a call whose legality and profitability were established
    // by a specialized transform.  
    // The caller is responsible for avoiding unbounded recursive expansion; 
    // this performs exactly one inline step.
    void inlineCallUnchecked(CallInst* call) { doInline(call); }

private:
    bool isInlineable(CallInst* call, Loop* callSiteLoop, SCEV* scev, int callSiteCount) const;

    void doInline(CallInst* call);

    // Move AllocaInst to entry block for better readability.
    static void AllocaHoist(Function* func);
};

}

#endif
