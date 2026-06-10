#ifndef FLATTENCFG_H
#define FLATTENCFG_H

#include "../../IR/Module.h"
#include "../../IR/IRBuilder.h"

namespace sysy {

class FlattenCFG {
public:
    FlattenCFG(Module* m) : TheModule(m) {}
    void run();
private:
    Module* TheModule;
    IRBuilder builder;

    void flattenRegion(Region* region, BasicBlock* loopHeader, BasicBlock* loopExit);

    void handleIf(IfInst* inst, BasicBlock* currentBB, BasicBlock* mergeBB, BasicBlock* loopHeader, BasicBlock* loopExit);
    void handleWhile(WhileInst* inst, BasicBlock* currentBB, BasicBlock* mergeBB);
};

}

#endif