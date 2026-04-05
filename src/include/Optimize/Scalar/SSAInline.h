#ifndef SSAINLINE_H
#define SSAINLINE_H

#include "IR/Instruction.h"
#include "IR/Module.h"
#include <map>

namespace sysy {

class SSAInline {
    Module* M;
    int threshold;

public:
    static bool isRecursive(Function* f);
    static int  countInsts(Function* f);
    static Value* remap(Value* v,
                        const std::map<Value*, Value*>& vmap,
                        const std::map<BasicBlock*, BasicBlock*>& bbMap);
    static Instruction* cloneNonPhiInst(Instruction* inst, BasicBlock* target,
                                        const std::map<Value*, Value*>& vmap,
                                        const std::map<BasicBlock*, BasicBlock*>& bbMap);
    explicit SSAInline(Module* m, int threshold = 200) : M(m), threshold(threshold) {}
    bool run();

private:
    bool isInlineable(Function* f) const;

    void doInline(CallInst* call);

    // Move AllocaInst to entry block for better readability.
    static void AllocaHoist(Function* func);
};

}

#endif
