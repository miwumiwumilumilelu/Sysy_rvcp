#ifndef INSTRUCTION_H
#define INSTRUCTION_H

#include "IR/Value.h"
#include "IR/Region.h"
#include <map>

namespace sysy {

class BasicBlock;
class Function;

class Instruction : public User {
public:
    enum OpID {
        Alloca, Load, Store,
        Add, Sub, Mul, Div, Mod,
        // x << n <-> x * (2^n)
        Shl, 
        // x >> n <-> ⌊x/2n⌋
        Ashr,
        And, Or, Xor,
        FAdd, FSub, FMul, FDiv,
        SIToFP, FPToSI,
        ICmp, FCmp, Br, Ret, Call,
        If, While, For, Break, Continue,
        GetElementPtr,
        Phi,
        // cond ? T : F
        Select,
        Flow
    };

    Instruction(Type* ty, OpID id, BasicBlock* parent);
    virtual ~Instruction();

    OpID getOpID() const { return OpCode; }
    bool isTerminator() const;

    BasicBlock* getParent() const { return Parent; }
    void setParent(BasicBlock* bb) { Parent = bb; }
    void dropAllOperands();
    void eraseInst();

    void addRegion(std::unique_ptr<class Region> region) {
        Regions.push_back(std::move(region));
    }
    Region* getRegion(int index) const { return Regions[index].get(); }
    const std::vector<std::unique_ptr<Region>>& getRegions() const {return Regions; }

    // Deep-clone this instruction, remapping operands through vmap.
    // Returns nullptr for instruction types that are not cloneable (e.g. Br, Ret, Phi).
    virtual Instruction* clone(std::map<Value*, Value*>& vmap);
    bool isTerminatingFlow() const;
    bool isPureCloneable() const;

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
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Call;
    }
};

class BinaryInst : public Instruction {
    std::string OpStr;
public:
    BinaryInst(OpID id, Value* lhs, Value* rhs, BasicBlock* parent);
    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

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
    Type* getAllocatedType() const { return AllocatedType; }
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Alloca;
    }
};

class LoadInst : public Instruction {
public:
    LoadInst(Value* ptr, BasicBlock* parent);
    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Load;
    }
};

class StoreInst : public Instruction {
public:
    StoreInst(Value* val, Value* ptr, BasicBlock* parent);
    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

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
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

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
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == ICmp;
    }
private:
    CmpOp Pred;
    std::string getPredStr() const;
};

class IfInst : public Instruction {
    std::vector<ResultValue*> Results;
public:
    IfInst(Value* cond, BasicBlock* parent);
    ~IfInst();

    Region* getThenRegion() { return getRegion(0); }
    Region* getElseRegion() { return getRegions().size() > 1 ? getRegion(1) : nullptr; }
    void addElseRegion();

    ResultValue* createResult(Type* ty);
    unsigned getNumResults() const { return Results.size(); }
    ResultValue* getResult(unsigned i) const { return Results[i]; }
    const std::vector<ResultValue*>& getResults() const { return Results; }

    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == If;
    }
};

class WhileInst : public Instruction {
    std::vector<ResultValue*> Results;
public:
    WhileInst(BasicBlock* parent);
    ~WhileInst();

    Region* getCondRegion() { return getRegion(0); }
    Region* getBodyRegion() { return getRegion(1); }

    ResultValue* createResult(Type* ty);
    unsigned getNumResults() const { return Results.size(); }
    ResultValue* getResult(unsigned i) const { return Results[i]; }
    const std::vector<ResultValue*>& getResults() const { return Results; }

    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == While;
    }
};

// while (cmp(load(ivAddr), stop)) { body; store(ivAddr, load(ivAddr)+step); }
//
// The body Region does NOT contain the IV update — that is implicit in ForInst.
// Operands: [start, stop, step, ivAddr]
class ForInst : public Instruction {
    ICmpInst::CmpOp Pred; // SLT / SLE / SGT / SGE
public:
    ForInst(Value* start, Value* stop, Value* step, Value* ivAddr,
            ICmpInst::CmpOp pred, BasicBlock* parent);

    Value* getStart() const { return getOperand(0); }
    Value* getStop() const { return getOperand(1); }
    Value* getStep() const { return getOperand(2); }
    Value* getIVAddr() const { return getOperand(3); }
    ICmpInst::CmpOp getPred() const { return Pred; }

    Region* getBodyRegion() { return getRegion(0); }

    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == For;
    }
};

class BreakInst : public Instruction {
public:
    BreakInst(BasicBlock* parent);
    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Break;
    }
};

class ContinueInst : public Instruction {
public:
    ContinueInst(BasicBlock* parent);
    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Continue;
    }
};

class GetElementPtrInst : public Instruction {
    Type* indexedType;

public:
    GetElementPtrInst(Value* base, Value* index, BasicBlock* parent);
    std::string toString() const override;
    Type* getIndexedType() const { return indexedType; }
    void setIndexedType(Type* ty) { indexedType = ty; }
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

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
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

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
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

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

    void removeIncomingAt(int index);

    void removeIncomingByBlock(BasicBlock* bb);
    
    std::string toString() const override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Phi;
    }
};

class SelectInst : public Instruction {
public:
    // operands: cond (0), T (1), F (2)
    SelectInst(Value* cond, Value* trueVal, Value* falseVal, BasicBlock* parent);
    // Type-explicit constructor
    // Used when cloning (T may be null at skeleton time).
    SelectInst(Type* ty, Value* cond, Value* trueVal, Value* falseVal, BasicBlock* parent);
    Value* getCond() const { return getOperand(0); }
    Value* getTrueVal() const { return getOperand(1); }
    Value* getFalseVal() const { return getOperand(2); }
    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Select;
    }
};

// FlowInst terminates a sub-region (IfInst branch or WhileInst body),
// flowing 0 or more values upward to the enclosing Hign inst.
class FlowInst : public Instruction {
public:
    FlowInst(std::vector<Value*> vals, BasicBlock* parent);

    unsigned getNumValues() const { return getNumOperands(); }
    Value* getValue(unsigned i) const { return getOperand(i); }

    std::string toString() const override;
    Instruction* clone(std::map<Value*, Value*>& vmap) override;

    static bool classof(const Value* v) {
        return isa<Instruction>(v) && cast<Instruction>(v)->getOpID() == Flow;
    }
};

}

#endif
