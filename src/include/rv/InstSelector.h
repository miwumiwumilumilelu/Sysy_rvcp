#ifndef INSTSELECTOR_H
#define INSTSELECTOR_H

#include "IR/Module.h"
#include "rv/MCModule.h"
#include <map>

namespace sysy {

class InstSelector {
public:
    MCModule* run(Module* irModule);

private:
    MCModule* curMCMod;
    MCFunc* curMCFunc;
    MCBlk* curMCBlk;

    // Vregister allocation.
    int nextVRegNo;
    MCOpnd createVReg();

    std::map<Value*, MCOpnd> val2opnd;
    std::map<BasicBlock*, MCBlk*> bbMap;

    MCOpnd getOpnd(Value* val);
    
    void selectFunction(Function* func);
    void selectBlock(BasicBlock* bb);
    void selectInstruction(Instruction* inst);
};

}

#endif