#ifndef IRBUILDER_H
#define IRBUILDER_H

#include "IR/Instruction.h"
#include "IR/Module.h"

namespace sysy {

class IRBuilder {
    BasicBlock *InsertPoint = nullptr;

public:
    void SetInsertPoint(BasicBlock *bb) { InsertPoint = bb; }
    BasicBlock *GetInsertPoint() { return InsertPoint; }

    template <typename InstT, typename... Args>
    InstT *Create(Args&&... args) {
        auto inst = new InstT(std::forward<Args>(args)..., InsertPoint);
        return inst;
    }

    ReturnInst *CreateRetVoid() {
        return Create<ReturnInst>(nullptr);
    }

    ReturnInst *CreateRet(Value *val) {
        return Create<ReturnInst>(val);
    }
    
    BranchInst *CreateBr(BasicBlock *dest) {
        return Create<BranchInst>(dest);
    }
};

}
#endif