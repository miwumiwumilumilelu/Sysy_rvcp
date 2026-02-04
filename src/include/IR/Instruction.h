#ifndef INSTRUCTION_H
#define INSTRUCTION_H

#include "IR/Value.h"
#include "IR/Region.h"

namespace sysy {

class BasicBlock;

class Instruction : public User {
public:
    enum OpID {
        Alloca, Load, Store,
        Add, Sub, Mul, Div,
        ICmp, Br, Ret, Call,
        If, While, Break, Continue
    };

    Instruction(Type* ty, OpID id, BasicBlock* parent);
    virtual ~Instruction();

    OpID getOpID() const { return OpCode; }
    bool isTerminator() const;

    BasicBlock* getParent() const { return Parent; }

    void addRegion(std::unique_ptr<class Region> region) {
        Regions.push_back(std::move(region));
    }
    Region* getRegion(int index) const { return Regions[index].get(); }
    const std::vector<std::unique_ptr<Region>>& getRegions() const {return Regions; }

private:
    OpID OpCode;
    BasicBlock* Parent;
    std::vector<std::unique_ptr<Region>> Regions;
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

class IfInst : public Instruction {
public:
    IfInst(Value* cond, BasicBlock* parent);
    Region* getThenRegion() { return getRegion(0); }
    Region* getElseRegion() { return getRegions().size() > 1 ? getRegion(1) : nullptr; }
    void addElseRegion();
    std::string toString() const override;
};

class WhileInst : public Instruction {
public:
    WhileInst(BasicBlock* parent);
    
    Region* getCondRegion() { return getRegion(0); }
    Region* getBodyRegion() { return getRegion(1); }
    std::string toString() const override;
};

class BreakInst : public Instruction {
public:
    BreakInst(BasicBlock* parent);
    std::string toString() const override;
};

class ContinueInst : public Instruction {
public:
    ContinueInst(BasicBlock* parent);
    std::string toString() const override;
};

}

#endif