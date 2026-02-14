#ifndef INSTRUCTION_H
#define INSTRUCTION_H

#include "IR/Value.h"
#include "IR/Region.h"

namespace sysy {

class BasicBlock;
class Function;

class Instruction : public User {
public:
    enum OpID {
        Alloca, Load, Store,
        Add, Sub, Mul, Div, Mod,
        FAdd, FSub, FMul, FDiv,
        SIToFP, FPToSI,
        ICmp, FCmp, Br, Ret, Call,
        If, While, Break, Continue,
        GetElementPtr,
        Phi
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

    static bool classof(const Value* v) {
        return v->getValueKind() == VK_Instruction;
    }

private:
    OpID OpCode;
    BasicBlock* Parent;
    std::vector<std::unique_ptr<Region>> Regions;
};

class CallInst : public Instruction {
public:
    CallInst(Function* func, std::vector<Value*> args, BasicBlock* parent);
    Function* getFunction() const;
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Call;
    }
};

class BinaryInst : public Instruction {
    std::string OpStr;
public:
    BinaryInst(OpID id, Value* lhs, Value* rhs, BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        if (!isa<Instruction>(v)) return false;
        OpID op = cast<Instruction>(v)->getOpID();
        return op >= Add && op <= FDiv;
    }
};

class AllocaInst : public Instruction {
    Type *AllocatedType;
public:
    AllocaInst(Type* ty, BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Alloca;
    }
};

class LoadInst : public Instruction {
public:
    LoadInst(Value* ptr, BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Load;
    }
};

class StoreInst : public Instruction {
public:
    StoreInst(Value* val, Value* ptr, BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Store;
    }
};

class ReturnInst : public Instruction {
public:
    ReturnInst(Value* val, BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Ret;
    }
};

class BranchInst : public Instruction {
public:
    // Unconditional jmp
    BranchInst(BasicBlock *dest, BasicBlock* parent);
    // Conditional jmp
    BranchInst(Value *cond, BasicBlock *ifTrue, BasicBlock *ifFalse, BasicBlock *parent);

    void replaceSuccessor(BasicBlock* oldBB, BasicBlock* newBB);

    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Br;
    }
};

class ICmpInst : public Instruction {
public:
    enum CmpOp { EQ, NE, SGT, SGE, SLT, SLE };
    ICmpInst(CmpOp op, Value* lhs, Value* rhs, BasicBlock* parent);
    CmpOp getPredicate() const { return Pred; }
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == ICmp;
    }
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

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == If;
    }
};

class WhileInst : public Instruction {
public:
    WhileInst(BasicBlock* parent);
    
    Region* getCondRegion() { return getRegion(0); }
    Region* getBodyRegion() { return getRegion(1); }
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == While;
    }
};

class BreakInst : public Instruction {
public:
    BreakInst(BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Break;
    }
};

class ContinueInst : public Instruction {
public:
    ContinueInst(BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Continue;
    }
};

class GetElementPtrInst : public Instruction {
public:
    GetElementPtrInst(Value* base, Value* index, BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == GetElementPtr;
    }
};

// Type casting：
// int -> float
// float -> int
class CastInst : public Instruction {
public:
    CastInst(OpID op, Value* val, Type* targetTy, BasicBlock* parent);
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) &&
               (cast<Instruction>(v)->getOpID() == SIToFP ||
                cast<Instruction>(v)->getOpID() == FPToSI);
    }
};

class FCmpInst : public Instruction {
public:
    // O represents Ordered (considering NaN)
    enum CmpOp { OEQ, ONE, OGT, OGE, OLT, OLE };
    FCmpInst(CmpOp op, Value* lhs, Value* rhs, BasicBlock* parent);
    CmpOp getPredicate() const { return Pred; }
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == FCmp;
    }
private:
    CmpOp Pred;
    std::string getPredStr() const;
};

class PhiInst : public Instruction {
public:
    PhiInst(Type* ty, BasicBlock* parent);

    void addIncoming(Value* val, BasicBlock* bb);

    void removeIncomingByBlock(BasicBlock* bb);
    
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Phi;
    }
};

}

#endif