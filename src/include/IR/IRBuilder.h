#ifndef IRBUILDER_H
#define IRBUILDER_H

#include "IR/Instruction.h"
#include "IR/Module.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include "IR/Region.h" 
#include <list>
#include <string>
#include <utility>

namespace sysy {

class IRBuilder {
private:
    BasicBlock *InsertPoint = nullptr;
    std::list<Instruction*>::iterator InsertBefore;
    bool HasInsertBefore = false;

public:
    IRBuilder() = default;

    void SetInsertPoint(BasicBlock *bb) {
        InsertPoint = bb;
        HasInsertBefore = false;
    }

    void SetInsertPoint(BasicBlock *bb, std::list<Instruction*>::iterator pos) {
        InsertPoint = bb;
        InsertBefore = pos;
        HasInsertBefore = true;
    }

    BasicBlock *GetInsertPoint() const {
        return InsertPoint;
    }

    BasicBlock *CreateBlock(const std::string& name, Region* parent) {
        return new BasicBlock(name, parent);
    }

    void Insert(Instruction* inst) {
        if (!inst || !InsertPoint)
            return;

        if (auto* oldParent = inst->getParent())
            oldParent->getInstructions().remove(inst);

        inst->setParent(InsertPoint);
        auto& insts = InsertPoint->getInstructions();
        if (HasInsertBefore)
            insts.insert(InsertBefore, inst);
        else
            insts.push_back(inst);
    }

    template <typename InstT, typename... Args>
    InstT *Create(Args&&... args) {
        return new InstT(std::forward<Args>(args)..., InsertPoint);
    }

    template <typename InstT, typename... Args>
    InstT *InsertNew(Args&&... args) {
        auto* inst = new InstT(std::forward<Args>(args)..., nullptr);
        Insert(inst);
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

} // namespace sysy

#endif
