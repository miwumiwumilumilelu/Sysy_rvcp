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

static std::string fmtOperand(Value* v) {
    if (auto c = dyn_cast<ConstantInt>(v)) {
        return std::to_string(c->getValue());
    }
    return v->getName();
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
    : Value(Type::getLabelTy(), VK_BasicBlock, name), Parent(parent) {
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
    ss << Name << ":\n";
    for (auto inst : InstList) {
        ss << addIndent(inst->toString(), 4) << "\n";
    }
    return ss.str();
}

Instruction::Instruction(Type *ty, OpID id, BasicBlock *parent) 
    : User(ty, VK_Instruction, ""), OpCode(id), Parent(parent) {
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
    return dyn_cast<Function>(getOperand(0));
}

std::string CallInst::toString() const {
    std::string str = "";
    if (!getType()->isVoid()) {
        str += Name + " = ";
    }
    str += "call " + getFunction()->getName() + "(";
    for (int i = 1; i < getNumOperands(); ++i) {
        if (i > 1) str += ", ";
        str += fmtOperand(getOperand(i));
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
    return Name + " = " + OpStr + " " + fmtOperand(getOperand(0)) + ", " + fmtOperand(getOperand(1));
}

AllocaInst::AllocaInst(Type *ty, BasicBlock *parent) 
    : Instruction(new PointerType(ty), Alloca, parent), AllocatedType(ty) {}

std::string AllocaInst::toString() const {
    return Name + " = alloca " + AllocatedType->toString();
}

LoadInst::LoadInst(Value *ptr, BasicBlock *parent) 
    : Instruction(
        dyn_cast<PointerType>(ptr->getType())->getPointeeType(),
        Load, 
        parent
      ) {
    addOperand(ptr);
}

std::string LoadInst::toString() const {
    return Name + " = load " + fmtOperand(getOperand(0));
}

StoreInst::StoreInst(Value *val, Value *ptr, BasicBlock *parent) 
    : Instruction(Type::getVoidTy(), Store, parent) {
    addOperand(val);
    addOperand(ptr);
}

std::string StoreInst::toString() const {
    return "store " + fmtOperand(getOperand(0)) + ", " + fmtOperand(getOperand(1));
}

ReturnInst::ReturnInst(Value *val, BasicBlock *parent) 
    : Instruction(Type::getVoidTy(), Ret, parent) {
    if (val) addOperand(val);
}

std::string ReturnInst::toString() const {
    if (getNumOperands() == 0) return "ret void";
    return "ret " + fmtOperand(getOperand(0));
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
    if (getNumOperands() == 1) return "br <" + getOperand(0)->getName() + ">";
    return "br " + fmtOperand(getOperand(0)) + ", <" + getOperand(1)->getName() + ">, <else = " + getOperand(2)->getName() + ">";
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
    return Name + " = icmp " + getPredStr() + " " + fmtOperand(getOperand(0)) + ", " + fmtOperand(getOperand(1));
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

GetElementPtrInst::GetElementPtrInst(Value* base, Value* index, BasicBlock* parent)
    : Instruction(base->getType(), GetElementPtr, parent) {
    addOperand(base);
    addOperand(index);

    if (auto ptrTy = dyn_cast<PointerType>(base->getType())) {
        if (auto arrTy = dyn_cast<ArrayType>(ptrTy->getPointeeType())) {
            Ty = new PointerType(arrTy->getElementType());
        }
    }
}

std::string GetElementPtrInst::toString() const {
    return Name + " = getelementptr " + getOperand(0)->getName() + ", " + getOperand(1)->getName();
}

PhiInst::PhiInst(Type *ty, BasicBlock *parent) 
    : Instruction(ty, Phi, parent) {}

void PhiInst::addIncoming(Value *val, BasicBlock *bb) {
    addOperand(val);
    addOperand(bb);
}

std::string PhiInst::toString() const {
    std::stringstream ss;
    // %1 = phi [ %val1, <bb1> ], [ %val2, <bb2> ]
    ss << Name << " = phi ";
    for (int i = 0; i < getNumOperands(); i += 2) {
        if (i > 0) ss << ", ";
        
        Value* val = getOperand(i);
        Value* bb = getOperand(i+1);
        
        std::string valName = val ? fmtOperand(val) : "null";
        std::string bbName = bb ? ("<" + bb->getName() + ">") : "null";
        
        ss << "[ " << valName << ", " << bbName << " ]";
    }
    return ss.str();
}

Function::Function(const std::string &name, Type *retTy) 
    : Value(retTy, VK_Function, name) {
    Body = std::make_unique<Region>(this);
}

BasicBlock* Function::getEntryBlock() {
    return Body->getEntryBlock();
}

std::string Function::toString() const {
    std::stringstream ss;
    ss << "define " << Ty->toString() << " @" << Name << "(";

    for (size_t i = 0; i < Args.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << Args[i]->getType()->toString() << " " << Args[i]->getName();
    }
    
    ss << ") {\n";
    for (auto bb : Body->getBlocks()) {
        ss << bb->toString();
    }
    ss << "}\n";
    return ss.str();
}

std::string ConstantArray::toString() const {
    std::stringstream ss;
    // [i32 1, i32 2]
    ss << "[";
    for (size_t i = 0; i < Consts.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << Consts[i]->getType()->toString() << " " << Consts[i]->toString();
    }
    ss << "]";
    return ss.str();
}

GlobalVariable::GlobalVariable(const std::string &name, Type *ty, Constant *initVal)
    : User(new PointerType(ty), VK_GlobalVariable, name), InitVal(initVal) {
}

std::string GlobalVariable::toString() const {
    std::stringstream ss;
    ss << "@" << Name << " = ";
    if (IsConst) ss << "constant "; else ss << "global ";
    
    Type* baseTy = dyn_cast<PointerType>(Ty)->getPointeeType();
    ss << baseTy->toString() << " ";
    
    if (InitVal) {
        ss << InitVal->toString();
    } else {
        ss << "zeroinitializer";
    }
    return ss.str();
}

std::string Module::print() {
    std::stringstream ss;
    for (auto g : Globals) {
        ss << g->toString() << "\n";
    }
    if (!Globals.empty()) ss << "\n";
    for (auto func : Functions) {
        ss << func->toString() << "\n";
    }
    return ss.str();
}
