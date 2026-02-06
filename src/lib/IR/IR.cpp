#include "IR/Module.h"
#include "IR/Instruction.h"
#include "IR/Region.h"
#include "IR/Type.h"
#include <iostream>
#include <sstream>

using namespace sysy;

static std::string addIndent(const std::string &str, int spaceCount) {
    std::stringstream ss(str);
    std::string line;
    std::stringstream out;
    std::string indent(spaceCount, ' ');
    
    while (std::getline(ss, line)) {
        out << indent << line << "\n";
    }
    
    std::string res = out.str();
    if (!res.empty() && str.back() != '\n') { 
        res.pop_back(); 
    }
    return res;
}

Type* Type::getIntTy() { static IntegerType t; return &t; }
Type* Type::getVoidTy() { static VoidType t; return &t; }
Type* Type::getFloatTy() { static FloatType t; return &t; }
Type* Type::getLabelTy() { static LabelType t; return &t; }

BasicBlock* Region::getEntryBlock() {
    if (Blocks.empty()) return nullptr;
    return Blocks.front();
}

void Region::addBlock(BasicBlock* bb) {
    Blocks.push_back(bb);
}

void Region::removeBlock(BasicBlock* bb) {
    Blocks.remove(bb);
}

BasicBlock::BasicBlock(const std::string &name, Region *parent) 
    : Value(Type::getLabelTy(), name), Parent(parent) {
    if (parent) parent->addBlock(this);
}

Function* BasicBlock::getParentFunc() const {
    if (!Parent) return nullptr;
    if (auto f = Parent->getParentFunc()) return f;
    if (auto i = Parent->getParentInst()) {
        if (i->getParent()) return i->getParent()->getParentFunc();
    }
    return nullptr;
}

std::string BasicBlock::toString() const {
    std::stringstream ss;
    ss << Name << ":\n"; // label:
    for (auto inst : InstList) {
        ss << addIndent(inst->toString(), 4) << "\n";
    }
    return ss.str();
}

Instruction::Instruction(Type *ty, OpID id, BasicBlock *parent) 
    : User(ty, ""), OpCode(id), Parent(parent) {
    if (parent) parent->addInstruction(this);
}

// unique_ptr automatically cleans up Regions.
Instruction::~Instruction() {}

bool Instruction::isTerminator() const {
    return OpCode == Br || OpCode == Ret || OpCode == Break || OpCode == Continue;
}

CallInst::CallInst(Function* func, std::vector<Value*> args, BasicBlock* parent)
    : Instruction(func->getType(), Call, parent) {
    addOperand(func);
    for (auto arg : args) {
        addOperand(arg);
    }
}

Function* CallInst::getFunction() const {
    return dynamic_cast<Function*>(getOperand(0));
}

std::string CallInst::toString() const {
    std::string str = "";
    if (!getType()->isVoid()) {
        str += Name + " = ";
    }
    str += "call " + getFunction()->getName() + "(";
    for (int i = 1; i < getNumOperands(); ++i) {
        if (i > 1) str += ", ";
        str += "i32 " + getOperand(i)->getName();
    }
    str += ")";
    return str;
}

BinaryInst::BinaryInst(OpID id, Value *lhs, Value *rhs, BasicBlock *parent) 
    : Instruction(Type::getIntTy(), id, parent) {
    addOperand(lhs);
    addOperand(rhs);
    switch(id) {
        case Add: OpStr = "add"; break;
        case Sub: OpStr = "sub"; break;
        case Mul: OpStr = "mul"; break;
        case Div: OpStr = "sdiv"; break; // signed div
        default: OpStr = "unknown"; break;
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
    : Instruction(Type::getIntTy(), Load, parent) { // Assume load i32 for the time being
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
    : Instruction(Type::getIntTy(), ICmp, parent), Pred(op) { // 注意: 实际上应该是 i1
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

IfInst::IfInst(Value* cond, BasicBlock* parent) 
    : Instruction(Type::getVoidTy(), If, parent) {
    addOperand(cond);
    addRegion(std::make_unique<Region>(this));
}

void IfInst::addElseRegion() {
    if (getRegions().size() < 2) {
        addRegion(std::make_unique<Region>(this));
    }
}

std::string IfInst::toString() const {
    std::stringstream ss;
    ss << "if (" << getOperand(0)->getName() << ") {\n";
    for (auto bb : getRegion(0)->getBlocks()) {
        ss << addIndent(bb->toString(), 2);
    }
    ss << "  }";
    if (getRegions().size() > 1) {
        ss << " else {\n";
        for (auto bb : getRegion(1)->getBlocks()) {
            ss << addIndent(bb->toString(), 2);
        }
        ss << "  }";
    }
    return ss.str();
}

WhileInst::WhileInst(BasicBlock* parent) 
    : Instruction(Type::getVoidTy(), While, parent) {
    addRegion(std::make_unique<Region>(this)); // Region 0: Cond
    addRegion(std::make_unique<Region>(this)); // Region 1: Body
}

std::string WhileInst::toString() const {
    std::stringstream ss;
    ss << "while {\n";
    for (auto bb : getRegion(0)->getBlocks()) ss << bb->toString();
    ss << "} do {\n";
    for (auto bb : getRegion(1)->getBlocks()) ss << bb->toString();
    ss << "}";
    return ss.str();
}

BreakInst::BreakInst(BasicBlock* parent) 
    : Instruction(Type::getVoidTy(), Break, parent) {}

std::string BreakInst::toString() const { 
    return "break"; 
}

ContinueInst::ContinueInst(BasicBlock* parent) 
    : Instruction(Type::getVoidTy(), Continue, parent) {}

std::string ContinueInst::toString() const { 
    return "continue"; 
}

Function::Function(const std::string &name, Type *retTy) 
    : Value(retTy, name) {
    Body = std::make_unique<Region>(this);
}

std::string Function::toString() const {
    std::stringstream ss;
    ss << "define " << Ty->toString() << " @" << Name << "() {\n";
    for (auto bb : Body->getBlocks()) {
        ss << bb->toString();
    }
    ss << "}\n";
    return ss.str();
}

std::string Module::print() {
    std::stringstream ss;
    for (auto func : Functions) {
        ss << func->toString() << "\n";
    }
    return ss.str();
}
