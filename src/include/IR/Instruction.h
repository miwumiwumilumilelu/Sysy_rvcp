#ifndef INSTRUCTION_H
#define INSTRUCTION_H

#include "IR/Value.h"

namespace sysy {

class BasicBlock;

class Instruction : public User {
public:
    enum OpID {
        Alloca, Load, Store,
        Add, Sub, Mul, Div,
        ICmp, Br, Ret, Call
    };

    Instruction(Type* ty, OpID id, BasicBlock* parent);

    OpID getOpID() const { return OpCode; }
    bool isTerminator() const { return OpCode == Br || OpCode == Ret; }

private:
    OpID OpCode;
    BasicBlock* Parent;
};

class BinaryInst : public Instruction {
    std::string OpStr;
public:
    BinaryInst(OpID id, Value* lhs, Value* rhs, BasicBlock* parent);
    std::string toString() const override;
};

class AllocaInst : public Instruction {
    Type *AllocatedType;
public:
    AllocaInst(Type* ty, BasicBlock* parent);
    std::string toString() const override;
};

class LoadInst : public Instruction {
public:
    LoadInst(Value* ptr, BasicBlock* parent);
    std::string toString() const override;
};

class StoreInst : public Instruction {
public:
    StoreInst(Value* val, Value* ptr, BasicBlock* parent);
    std::string toString() const override;
};

class ReturnInst : public Instruction {
public:
    ReturnInst(Value* val, BasicBlock* parent);
    std::string toString() const override;
};

class BranchInst : public Instruction {
public:
    // Unconditional jmp
    BranchInst(BasicBlock *dest, BasicBlock* parent);
    // Conditional jmp
    BranchInst(Value *cond, BasicBlock *ifTrue, BasicBlock *ifFalse, BasicBlock *parent);

    std::string toString() const override;
};

class ICmpInst : public Instruction {
public:
    enum CmpOp { EQ, NE, SGT, SGE, SLT, SLE };
    ICmpInst(CmpOp op, Value* lhs, Value* rhs, BasicBlock* parent);
    std::string toString() const override;
private:
    CmpOp Pred;
    std::string getPredStr() const;
};

}

#endif