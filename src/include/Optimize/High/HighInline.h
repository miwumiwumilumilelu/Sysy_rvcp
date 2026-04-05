#ifndef HIGHINLINE_H
#define HIGHINLINE_H

#include "IR/Instruction.h"
#include "IR/Module.h"
#include <map>
#include <vector>

namespace sysy {

class HighInline {
    Module* M;
    int threshold;

    static int countInsts(Region* region);
    static void collectReturns(Region* region, std::vector<ReturnInst*>& rets);
    static bool atBack(Instruction* inst);
    static Instruction* tailParent(ReturnInst* ret);
    static void erase(Instruction* inst);
    static bool foldTailReturns(Function* f);
    bool isInlineable(Function* f) const;

    static Instruction* cloneFlatInst(
        Instruction* inst,
        BasicBlock* target,
        const std::map<Value*, Value*>& vmap,
        const std::map<BasicBlock*, BasicBlock*>& bbMap);
    static void appendInst(BasicBlock* bb, Instruction* inst);
    static void cloneRegion(Region* src,
                            Region* dst,
                            std::map<Value*, Value*>& vmap);
    static Instruction* cloneStructuredInst(
        Instruction* inst,
        std::map<Value*, Value*>& vmap);
    void doInline(CallInst* call);

public:
    explicit HighInline(Module* m, int threshold = 200) : M(m), threshold(threshold) {}
    bool run();
};

}

#endif
