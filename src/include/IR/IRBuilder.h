#ifndef IRBUILDER_H
#define IRBUILDER_H

#include "IR/Instruction.h"
#include "IR/Module.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include "IR/Region.h" 

namespace sysy {

class IRBuilder {
private:
    BasicBlock *InsertPoint = nullptr;

public:
    IRBuilder() = default;

    void SetInsertPoint(BasicBlock *bb) {
        InsertPoint = bb;
    }

    BasicBlock *GetInsertPoint() const {
        return InsertPoint;
    }

    template <typename InstT, typename... Args>
    InstT *Create(Args&&... args) {
        return new InstT(std::forward<Args>(args)..., InsertPoint);
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

} // namespace sysy

#endif