#ifndef HIGHMEM2REG_H
#define HIGHMEM2REG_H

/*
 HighMem2Reg promotes scalar allocas to SSA 
 while the IR is still in nested IfInst/WhileInst form, before FlattenCFG.
*/

#include "IR/Module.h"
#include "IR/Instruction.h"
#include <set>
#include <map>
#include <vector>

namespace sysy {
    
// IfInst: inserts FlowInst in then/else regions and creates ResultValues.
// WhileInst: inserts FlowInst at body-exit and creates loop-carried ResultValues.
// FlattenCFG then lowers FlowInst + ResultValue into PhiInst on the flat CFG.
class HighMem2Reg {
public:
    HighMem2Reg(Module* m) : TheModule(m) {}
    void run();

private:
    using ValMap = std::map<AllocaInst*, Value*>;

    Module* TheModule;
    std::set<AllocaInst*> Promotable;
    int Counter = 0;

    std::string nextName(const char* prefix);
    Value* getZero(AllocaInst* ai);

    void processFunction(Function* func);
    void processRegion(Region* region, ValMap& vals);
    void processBlock(BasicBlock* bb, ValMap& vals);
    void processIfInst(IfInst* inst, ValMap& vals);
    void processWhileInst(WhileInst* inst, ValMap& vals);

    bool isPromotable(AllocaInst* ai);
    void collectPromotable(Function* func);
    void findWritten(Region* region, std::set<AllocaInst*>& written);
    void markBadAllocas(Region* region, std::set<AllocaInst*>& bad);

    bool insertFlowInst(Region* region, const std::vector<Value*>& vals);
    static void eraseInst(Instruction* inst);
};

}

#endif
