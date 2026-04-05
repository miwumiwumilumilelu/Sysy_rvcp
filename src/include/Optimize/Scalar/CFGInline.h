#ifndef CFGINLINE_H
#define CFGINLINE_H

#include "IR/Instruction.h"
#include "IR/Module.h"
#include <map>

namespace sysy {

// Flat-CFG inliner before Mem2Reg.
class CFGInline {
    Module* M;
    int threshold;

    bool isInlineable(Function* f) const;

    static Value* remap(Value* v,
                        const std::map<Value*, Value*>& vmap,
                        const std::map<BasicBlock*, BasicBlock*>& bbMap);

    static Instruction* cloneInst(Instruction* inst, BasicBlock* target,
                                    const std::map<Value*, Value*>& vmap,
                                    const std::map<BasicBlock*, BasicBlock*>& bbMap);

    void doInline(CallInst* call);
    static void AllocaHoist(Function* func);

public:
    explicit CFGInline(Module* m, int threshold = 200) : M(m), threshold(threshold) {}
    bool run();
};

}

#endif
