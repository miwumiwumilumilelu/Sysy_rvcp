#include "IR/Module.h"
#include "IR/Type.h"
#include "IR/Instruction.h"

using namespace sysy;

// Types
Type* Type::getIntTy() { static IntegerType t; return &t; }
Type* Type::getVoidTy() { static VoidType t; return &t; }
Type* Type::getFloatTy() { static FloatType t; return &t; }
Type* Type::getLabelTy() { static LabelType t; return &t; }

// Instructions
Instruction::Instruction(Type *ty, OpID id, BasicBlock *parent) 
    : User(ty, ""), OpCode(id), Parent(parent) {
    if (parent) parent->addInstruction(this);
}

BinaryInst::BinaryInst(OpID id, Value *lhs, Value *rhs, BasicBlock *parent) 
    : Instruction(Type::getIntTy(), id, parent) {
    addOperand(lhs);
    addOperand(rhs);
    switch(id) {
        case Add: OpStr = "add"; break;
        case Sub: OpStr = "sub"; break;
        case Mul: OpStr = "mul"; break;
        case Div: OpStr = "sdiv"; break;
        default: OpStr = "?"; break;
    }
}

std::string BinaryInst::toString() const {
    return Name + " = " + OpStr + " i32 " + getOperand(0)->getName() + ", " + getOperand(1)->getName();
}

AllocaInst::AllocaInst(Type *ty, BasicBlock *parent) 
    : Instruction(new PointerType(ty), Alloca, parent), AllocatedType(ty) {}

std::string AllocaInst::toString() const {
    return Name + " = alloca " + AllocatedType->toString();
}

LoadInst::LoadInst(Value *ptr, BasicBlock *parent) 
    : Instruction(Type::getIntTy(), Load, parent) {
    addOperand(ptr);
}

std::string LoadInst::toString() const {
    return Name + " = load i32, i32* " + getOperand(0)->getName();
}

StoreInst::StoreInst(Value *val, Value *ptr, BasicBlock *parent) 
    : Instruction(Type::getVoidTy(), Store, parent) {
    addOperand(val);
    addOperand(ptr);
}

std::string StoreInst::toString() const {
    return "store i32 " + getOperand(0)->getName() + ", i32* " + getOperand(1)->getName();
}

ReturnInst::ReturnInst(Value *val, BasicBlock *parent) 
    : Instruction(Type::getVoidTy(), Ret, parent) {
    if (val) addOperand(val);
}

std::string ReturnInst::toString() const {
    if (getNumOperands() == 0) return "ret void";
    return "ret i32 " + getOperand(0)->getName();
}

BranchInst::BranchInst(BasicBlock *dest, BasicBlock *parent) 
    : Instruction(Type::getVoidTy(), Br, parent) {
    addOperand(dest);
}

BranchInst::BranchInst(Value *cond, BasicBlock *ifTrue, BasicBlock *ifFalse, BasicBlock *parent)
    : Instruction(Type::getVoidTy(), Br, parent) {
    addOperand(cond);
    addOperand(ifTrue);
    addOperand(ifFalse);
}

std::string BranchInst::toString() const {
    if (getNumOperands() == 1) return "br label %" + getOperand(0)->getName();
    return "br i1 " + getOperand(0)->getName() + ", label %" + getOperand(1)->getName() + ", label %" + getOperand(2)->getName();
}

ICmpInst::ICmpInst(CmpOp op, Value *lhs, Value *rhs, BasicBlock *parent)
    : Instruction(Type::getIntTy(), ICmp, parent), Pred(op) {
    addOperand(lhs);
    addOperand(rhs);
}

std::string ICmpInst::getPredStr() const {
    switch(Pred) {
        case EQ: return "eq"; case NE: return "ne";
        case SGT: return "sgt"; case SGE: return "sge";
        case SLT: return "slt"; case SLE: return "sle";
    }
    return "";
}

std::string ICmpInst::toString() const {
    return Name + " = icmp " + getPredStr() + " i32 " + getOperand(0)->getName() + ", " + getOperand(1)->getName();
}

// Module Structure
BasicBlock::BasicBlock(const std::string &name, Function *parent) 
    : Value(Type::getLabelTy(), name), Parent(parent) {
    if (parent) parent->addBlock(this);
}

std::string BasicBlock::toString() const {
    std::stringstream ss;
    ss << Name << ":\n";
    for (auto inst : InstList) ss << "  " << inst->toString() << "\n";
    return ss.str();
}

std::string Function::toString() const {
    std::stringstream ss;
    ss << "define i32 @" << Name << "() {\n";
    for (auto bb : Blocks) ss << bb->toString();
    ss << "}\n";
    return ss.str();
}

std::string Module::print() {
    std::stringstream ss;
    for (auto func : Functions) ss << func->toString() << "\n";
    return ss.str();
}