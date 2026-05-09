#include "IR/Module.h"
#include "IR/Instruction.h"
#include "IR/Region.h"
#include "IR/Type.h"
#include <iostream>
#include <map>
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
    if (!v) return "null";

    if (auto ci = dyn_cast<ConstantInt>(v)) {
        return std::to_string(ci->getValue());
    }
    else if (auto cf = dyn_cast<ConstantFloat>(v)) {
        return std::to_string(cf->getValue());
    }

    std::string name = v->getName();
    if (!name.empty()) return name;

    std::ostringstream os;
    if (auto* inst = dyn_cast<Instruction>(v)) {
        os << "<unnamed-inst@" << static_cast<const void*>(inst) << ">";
        return os.str();
    }
    os << "<unnamed-val@" << static_cast<const void*>(v) << ">";
    return os.str();
}

static std::string binarySymbol(Instruction::OpID op) {
    switch (op) {
        case Instruction::Add:  return "+";
        case Instruction::Sub:  return "-";
        case Instruction::Mul:  return "*";
        case Instruction::Div:  return "/";
        case Instruction::Mod:  return "%";
        case Instruction::Shl:  return "<<";
        case Instruction::Ashr: return ">>";
        case Instruction::And:  return "&";
        case Instruction::FAdd: return "+.f";
        case Instruction::FSub: return "-.f";
        case Instruction::FMul: return "*.f";
        case Instruction::FDiv: return "/.f";
        default:                return "?";
    }
}

static std::string icmpSymbol(ICmpInst::CmpOp pred) {
    switch (pred) {
        case ICmpInst::EQ:  return "==.i";
        case ICmpInst::NE:  return "!=.i";
        case ICmpInst::SGT: return ">.i";
        case ICmpInst::SGE: return ">=.i";
        case ICmpInst::SLT: return "<.i";
        case ICmpInst::SLE: return "<=.i";
    }
    return "?.i";
}

static std::string fcmpSymbol(FCmpInst::CmpOp pred) {
    switch (pred) {
        case FCmpInst::OEQ: return "==.f";
        case FCmpInst::ONE: return "!=.f";
        case FCmpInst::OGT: return ">.f";
        case FCmpInst::OGE: return ">=.f";
        case FCmpInst::OLT: return "<.f";
        case FCmpInst::OLE: return "<=.f";
    }
    return "?.f";
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

void Instruction::dropAllOperands() {
    for (int i = 0; i < getNumOperands(); ++i)
        setOperand(i, nullptr);
}

void Instruction::eraseInst() {
    if (Parent)
        Parent->getInstructions().remove(this);
    dropAllOperands();
    Parent = nullptr;
    delete this;
}

bool Instruction::isTerminator() const {
    return OpCode == Br || OpCode == Ret || OpCode == Break || OpCode == Continue || OpCode == Flow;
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
    : Instruction((id >= FAdd && id <= FDiv) ? Type::getFloatTy() : Type::getIntTy(), id, parent) {
    addOperand(lhs);
    addOperand(rhs);
    switch(id) {
        case Add: OpStr = "add"; break;
        case Sub: OpStr = "sub"; break;
        case Mul: OpStr = "mul"; break;
        case Div: OpStr = "sdiv"; break; // signed div
        case Mod: OpStr = "srem"; break;
        case Shl:  OpStr = "shl";  break;
        case Ashr: OpStr = "ashr"; break;
        case And:  OpStr = "and";  break;
        case FAdd: OpStr = "fadd"; break;
        case FSub: OpStr = "fsub"; break;
        case FMul: OpStr = "fmul"; break;
        case FDiv: OpStr = "fdiv"; break;
        default: OpStr = "unknown"; break;
    }
}

std::string BinaryInst::toString() const {
    return Name + " = " + fmtOperand(getOperand(0)) + " " + binarySymbol(getOpID()) + " " +
           fmtOperand(getOperand(1));
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

void BranchInst::replaceSuccessor(BasicBlock* oldBB, BasicBlock* newBB) {
    for (size_t i = 0; i < Operands.size(); ++i) {
        if (Operands[i] == oldBB) {
            setOperand(i, newBB);
        }
    }
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
    return Name + " = " + fmtOperand(getOperand(0)) + " " + icmpSymbol(Pred) + " " +
           fmtOperand(getOperand(1));
}

IfInst::IfInst(Value* cond, BasicBlock* parent) 
    : Instruction(Type::getVoidTy(), If, parent) {
    addOperand(cond);
    addRegion(std::make_unique<Region>(this));
}

IfInst::~IfInst() {
    for (auto r : Results) delete r;
}

void IfInst::addElseRegion() {
    if (getRegions().size() < 2) {
        addRegion(std::make_unique<Region>(this));
    }
}

ResultValue* IfInst::createResult(Type* ty) {
    auto* rv = new ResultValue(ty, this, Results.size());
    Results.push_back(rv);
    return rv;
}

std::string IfInst::toString() const {
    std::stringstream ss;
    if (!Results.empty()) {
        for (unsigned i = 0; i < Results.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << Results[i]->getName();
        }
        ss << " = ";
    }
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

WhileInst::~WhileInst() {
    for (auto r : Results) delete r;
}

ResultValue* WhileInst::createResult(Type* ty) {
    auto* rv = new ResultValue(ty, this, Results.size());
    Results.push_back(rv);
    return rv;
}

std::string WhileInst::toString() const {
    std::stringstream ss;
    if (!Results.empty()) {
        for (unsigned i = 0; i < Results.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << Results[i]->getName();
        }
        ss << " = ";
    }
    ss << "while {\n";
    for (auto bb : getRegion(0)->getBlocks()) ss << bb->toString();
    ss << "} do {\n";
    for (auto bb : getRegion(1)->getBlocks()) ss << bb->toString();
    ss << "}";
    return ss.str();
}

ForInst::ForInst(Value* start, Value* stop, Value* step, Value* ivAddr,
                ICmpInst::CmpOp pred, BasicBlock* parent)
    : Instruction(Type::getVoidTy(), For, parent), Pred(pred) {
    addOperand(start);
    addOperand(stop);
    addOperand(step);
    addOperand(ivAddr);
    addRegion(std::make_unique<Region>(this)); // body
}

static const char* predName(ICmpInst::CmpOp p) {
    switch (p) {
        case ICmpInst::SLT: return "<.i";
        case ICmpInst::SLE: return "<=.i";
        case ICmpInst::SGT: return ">.i";
        case ICmpInst::SGE: return ">=.i";
        default: return "?.i";
    }
}

std::string ForInst::toString() const {
    std::stringstream ss;
    ss << "for (store " << getIVAddr()->getName() << ", " << fmtOperand(getStart())
        << "; load " << getIVAddr()->getName() << " " << predName(Pred)
        << " " << fmtOperand(getStop())
        << "; load " << getIVAddr()->getName() << " += " << fmtOperand(getStep()) << ") {\n";
    for (auto bb : getRegion(0)->getBlocks())
        ss << bb->toString();
    ss << "}";
    return ss.str();
}

FlowInst::FlowInst(std::vector<Value*> vals, BasicBlock* parent)
    : Instruction(Type::getVoidTy(), Flow, parent) {
    for (auto v : vals) addOperand(v);
}

std::string FlowInst::toString() const {
    std::stringstream ss;
    ss << "flow";
    for (int i = 0; i < getNumOperands(); ++i) {
        ss << (i == 0 ? " " : ", ") << fmtOperand(getOperand(i));
    }
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
    : Instruction(base->getType(), GetElementPtr, parent), indexedType(nullptr) {
    addOperand(base);
    addOperand(index);

    if (auto ptrTy = dyn_cast<PointerType>(base->getType())) {
        indexedType = ptrTy->getPointeeType();
        if (auto arrTy = dyn_cast<ArrayType>(ptrTy->getPointeeType())) {
            Ty = new PointerType(arrTy->getElementType());
        }
    }
}

std::string GetElementPtrInst::toString() const {
    return Name + " = " + fmtOperand(getOperand(0)) + "[" + fmtOperand(getOperand(1)) + "]";
}

CastInst::CastInst(OpID op, Value* val, Type* targetTy, BasicBlock* parent)
    : Instruction(targetTy, op, parent) {
    addOperand(val);
}

std::string CastInst::toString() const {
    std::string opStr = (getOpID() == SIToFP) ? "sitofp" : "fptosi";
    // %2 = sitofp %1
    return Name + " = " + opStr + " " + fmtOperand(getOperand(0));
}

FCmpInst::FCmpInst(CmpOp op, Value *lhs, Value *rhs, BasicBlock *parent) 
    : Instruction(Type::getIntTy(), FCmp, parent), Pred(op) {
    addOperand(lhs);
    addOperand(rhs);
}

std::string FCmpInst::getPredStr() const {
    switch(Pred) {
        case OEQ: return "oeq"; case ONE: return "one";
        case OGT: return "ogt"; case OGE: return "oge";
        case OLT: return "olt"; case OLE: return "ole";
    }
    return "";
}

std::string FCmpInst::toString() const {
    return Name + " = " + fmtOperand(getOperand(0)) + " " + fcmpSymbol(Pred) + " " +
           fmtOperand(getOperand(1));
}

PhiInst::PhiInst(Type *ty, BasicBlock *parent) 
    : Instruction(ty, Phi, parent) {}

void PhiInst::addIncoming(Value *val, BasicBlock *bb) {
    addOperand(val);
    addOperand(bb);
}

void PhiInst::removeIncomingAt(int index) {
    if (index < 0 || index + 1 >= static_cast<int>(Operands.size()))
        return;
    Value* val = Operands[index];
    Value* block = Operands[index + 1];
    if (val) val->removeUser(this);
    if (block) block->removeUser(this);
    Operands.erase(Operands.begin() + index, Operands.begin() + index + 2);
}

void PhiInst::removeIncomingByBlock(BasicBlock* bb) {
    for (auto it = Operands.begin(); it != Operands.end(); ) {
        if ((it + 1) != Operands.end() && *(it + 1) == bb) {
            Value* val = *it;
            Value* blcok = *(it + 1);
            if (val) val->removeUser(this);
            if (blcok) blcok->removeUser(this);

            it = Operands.erase(it, it + 2);
        } else {
            it += 2;
        }
    }
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
    bool allZero = true;
    for (auto c : Consts) {
        if (!isa<ConstantZero>(c) && 
            !(isa<ConstantInt>(c) && cast<ConstantInt>(c)->getValue() == 0) &&
            !(isa<ConstantFloat>(c) && cast<ConstantFloat>(c)->getValue() == 0.0)) {
            allZero = false;
            break;
        }
    }
    if (allZero) return "zeroinitializer";
    ss << "[";
    int count = 0;
    for (size_t i = 0; i < Consts.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << Consts[i]->getType()->toString() << " " << Consts[i]->toString();
        count++;
        if (count >= 16) {
            ss << ", ... (" << Consts.size() - count << " more elements)";
            break;
        }
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

// ===== Clone infrastructure =====

static Value* remapVal(Value* v, const std::map<Value*, Value*>& vmap) {
    if (!v) return nullptr;
    auto it = vmap.find(v);
    return it != vmap.end() ? it->second : v;
}

static void mergeVmap(std::map<Value*, Value*>& dst,
                      const std::map<Value*, Value*>& src) {
    for (auto& [k, v] : src)
        dst[k] = v;
}

bool Instruction::isTerminatingFlow() const {
    return OpCode == Break || OpCode == Continue || OpCode == Ret || OpCode == Br;
}

bool Instruction::isPureCloneable() const {
    switch (OpCode) {
    case Add: case Sub: case Mul: case Div: case Mod:
    case Shl: case Ashr: case And:
    case FAdd: case FSub: case FMul: case FDiv:
    case ICmp: case FCmp:
    case SIToFP: case FPToSI:
    case Load: case GetElementPtr:
        return true;
    default:
        return false;
    }
}

Instruction* Instruction::clone(std::map<Value*, Value*>&) { return nullptr; }

Instruction* BinaryInst::clone(std::map<Value*, Value*>& vmap) {
    return new BinaryInst(getOpID(), remapVal(getOperand(0), vmap),
                          remapVal(getOperand(1), vmap), nullptr);
}

Instruction* AllocaInst::clone(std::map<Value*, Value*>&) {
    return new AllocaInst(getAllocatedType(), nullptr);
}

Instruction* LoadInst::clone(std::map<Value*, Value*>& vmap) {
    return new LoadInst(remapVal(getOperand(0), vmap), nullptr);
}

Instruction* StoreInst::clone(std::map<Value*, Value*>& vmap) {
    return new StoreInst(remapVal(getOperand(0), vmap),
                         remapVal(getOperand(1), vmap), nullptr);
}

Instruction* ICmpInst::clone(std::map<Value*, Value*>& vmap) {
    return new ICmpInst(Pred, remapVal(getOperand(0), vmap),
                        remapVal(getOperand(1), vmap), nullptr);
}

Instruction* FCmpInst::clone(std::map<Value*, Value*>& vmap) {
    return new FCmpInst(Pred, remapVal(getOperand(0), vmap),
                        remapVal(getOperand(1), vmap), nullptr);
}

Instruction* CastInst::clone(std::map<Value*, Value*>& vmap) {
    return new CastInst(getOpID(), remapVal(getOperand(0), vmap), getType(), nullptr);
}

Instruction* GetElementPtrInst::clone(std::map<Value*, Value*>& vmap) {
    auto* g = new GetElementPtrInst(remapVal(getOperand(0), vmap),
                                    remapVal(getOperand(1), vmap), nullptr);
    g->setIndexedType(indexedType);
    return g;
}

Instruction* CallInst::clone(std::map<Value*, Value*>& vmap) {
    std::vector<Value*> args;
    for (int i = 1; i < getNumOperands(); ++i)
        args.push_back(remapVal(getOperand(i), vmap));
    return new CallInst(getFunction(), args, nullptr);
}

Instruction* BreakInst::clone(std::map<Value*, Value*>&) {
    return new BreakInst(nullptr);
}

Instruction* ContinueInst::clone(std::map<Value*, Value*>&) {
    return new ContinueInst(nullptr);
}

Instruction* FlowInst::clone(std::map<Value*, Value*>& vmap) {
    std::vector<Value*> vals;
    for (int i = 0; i < getNumOperands(); ++i)
        vals.push_back(remapVal(getOperand(i), vmap));
    return new FlowInst(vals, nullptr);
}

Instruction* IfInst::clone(std::map<Value*, Value*>& vmap) {
    auto baseMap = vmap;
    auto* c = new IfInst(remapVal(getOperand(0), vmap), nullptr);
    c->setName(getName());
    if (getElseRegion()) c->addElseRegion();
    for (auto* rv : Results) {
        auto* nrv = c->createResult(rv->getType());
        nrv->setName(rv->getName());
        vmap[rv] = nrv;
        baseMap[rv] = nrv;
    }
    auto thenMap = baseMap;
    getThenRegion()->clone(c->getThenRegion(), thenMap);
    mergeVmap(vmap, thenMap);
    if (getElseRegion()) {
        auto elseMap = baseMap;
        getElseRegion()->clone(c->getElseRegion(), elseMap);
        mergeVmap(vmap, elseMap);
    }
    vmap[this] = c;
    return c;
}

Instruction* WhileInst::clone(std::map<Value*, Value*>& vmap) {
    auto baseMap = vmap;
    auto* c = new WhileInst(nullptr);
    c->setName(getName());
    for (auto* rv : Results) {
        auto* nrv = c->createResult(rv->getType());
        nrv->setName(rv->getName());
        vmap[rv] = nrv;
        baseMap[rv] = nrv;
    }
    auto condMap = baseMap;
    auto bodyMap = baseMap;
    getCondRegion()->clone(c->getCondRegion(), condMap);
    getBodyRegion()->clone(c->getBodyRegion(), bodyMap);
    mergeVmap(vmap, condMap);
    mergeVmap(vmap, bodyMap);
    vmap[this] = c;
    return c;
}

Instruction* ForInst::clone(std::map<Value*, Value*>& vmap) {
    auto* c = new ForInst(remapVal(getStart(), vmap), remapVal(getStop(), vmap),
                          remapVal(getStep(), vmap), remapVal(getIVAddr(), vmap),
                          Pred, nullptr);
    c->setName(getName());
    auto bodyMap = vmap;
    getBodyRegion()->clone(c->getBodyRegion(), bodyMap);
    mergeVmap(vmap, bodyMap);
    vmap[this] = c;
    return c;
}

void Region::clone(Region* dst, std::map<Value*, Value*>& vmap) {
    std::map<BasicBlock*, BasicBlock*> bbMap;
    std::vector<std::pair<PhiInst*, PhiInst*>> pendingPhis;

    for (auto* bb : Blocks)
        bbMap[bb] = new BasicBlock(bb->getName(), dst);

    for (auto* bb : Blocks) {
        auto* dstBB = bbMap[bb];
        for (auto* inst : bb->getInstructions()) {
            if (auto* phi = dyn_cast<PhiInst>(inst)) {
                auto* cphi = new PhiInst(phi->getType(), nullptr);
                cphi->setName(phi->getName());
                cphi->setParent(dstBB);
                dstBB->addInstruction(cphi);
                vmap[phi] = cphi;
                pendingPhis.push_back({phi, cphi});
                continue;
            }
            auto* cloned = inst->clone(vmap);
            if (!cloned) continue;
            cloned->setName(inst->getName());
            cloned->setParent(dstBB);
            dstBB->addInstruction(cloned);
            if (!cloned->getType()->isVoid())
                vmap[inst] = cloned;
        }
    }

    for (auto [orig, cphi] : pendingPhis) {
        for (int i = 0; i < orig->getNumOperands(); i += 2) {
            Value* val = remapVal(orig->getOperand(i), vmap);
            auto* bb = dyn_cast<BasicBlock>(orig->getOperand(i + 1));
            auto bit = bbMap.find(bb);
            BasicBlock* newBB = (bit != bbMap.end()) ? bit->second : bb;
            cphi->addIncoming(val, newBB);
        }
    }
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
