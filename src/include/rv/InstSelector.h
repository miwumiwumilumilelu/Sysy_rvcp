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
    MCOpnd createVReg(bool isFloat = false);

    std::map<Value*, MCOpnd> val2opnd;
    std::map<BasicBlock*, MCBlk*> bbMap;

    MCOpnd getOpnd(Value* val);
    
    void selectFunction(Function* func);
    void selectBlock(BasicBlock* bb);
    void selectInstruction(Instruction* inst);

    // Instruction selection helpers
    void selectRet(class ReturnInst* inst);
    void selectBranch(class BranchInst* inst);
    void selectBinaryOp(class BinaryInst* inst);
    void selectFBinaryOp(class BinaryInst* inst);
    void selectAlloca(class AllocaInst* inst);
    void selectLoad(class LoadInst* inst);
    void selectStore(class StoreInst* inst);
    void selectICmp(class ICmpInst* inst);
    void selectFCmp(class FCmpInst* inst);
    void selectCall(class CallInst* inst);
    void selectCast(class CastInst* inst);
    void selectGetElementPtr(class GetElementPtrInst* inst);
    void selectPhi(class PhiInst* inst);
};

}

#endif