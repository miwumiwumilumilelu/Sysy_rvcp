#ifndef CONSTANTFOLD_H
#define CONSTANTFOLD_H

#include "../../IR/Module.h"
#include "../../IR/Instruction.h"
#include "../../IR/Value.h"

namespace sysy {

class ConstantFold {
public:
    ConstantFold(Module* m) : TheModule(m) {}
    bool run();
private:
    Module* TheModule;

    bool foldInstruction(Instruction* inst, BasicBlock* currentBB);

    Constant* computeBinary(Instruction::OpID op, Constant* lhs, Constant* rhs);
    Constant* computeICmp(ICmpInst::CmpOp pred, Constant* lhs, Constant* rhs);
    Constant* computeFCmp(FCmpInst::CmpOp pred, Constant* lhs, Constant* rhs);
};

}

#endif