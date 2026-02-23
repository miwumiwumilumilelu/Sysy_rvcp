#include "IR/IRGen.h"
#include "IR/Type.h"
#include "IR/Module.h"
#include <iostream>

using namespace sysy;

static int getDimCount(Type* ty) {
    if (ty->isPointer()) {
        return 1 + getDimCount(dyn_cast<PointerType>(ty)->getPointeeType());
    }
    if (ty->isArray()) {
        return 1 + getDimCount(dyn_cast<ArrayType>(ty)->getElementType());
    }
    return 0;
}

IRGen::IRGen() {
    TheModule = std::make_unique<Module>();
    CurrentFunc = nullptr;
}

void IRGen::defineVar(const std::string &name, Value *val) {
    if (Scopes.empty()) return;
    Scopes.back()[name] = val;
}

Value* IRGen::lookupVar(const std::string &name) {
    for (auto it = Scopes.rbegin(); it != Scopes.rend(); ++it) {
        if (it->find(name) != it->end()) return it->at(name);
    }
    return nullptr; 
}

Value* IRGen::castTo(Value* val, Type* targetTy) {
    if (val->getType() == targetTy) {
        return val;
    }
    // int -> float
    if (targetTy->isFloat() && val->getType()->isInt()) {
        auto inst = builder.Create<CastInst>(Instruction::SIToFP, val, targetTy);
        inst->setName(nextValueName());
        return inst;
    }
    // float -> int
    if (targetTy->isInt() && val->getType()->isFloat()) {
        auto inst = builder.Create<CastInst>(Instruction::FPToSI, val, targetTy);
        inst->setName(nextValueName());
        return inst;
    }
    return val;
}

Value* IRGen::toCondition(Value* cond) {
    if (!cond) return nullptr;

    // It is already a comparison instruction in itself, and it is already a condition, so go back directly.
    if (auto inst = dyn_cast<Instruction>(cond)) {
        if (inst->getOpID() == Instruction::ICmp || inst->getOpID() == Instruction::FCmp) {
            return cond;
        }
    }

    // Compare with 0.
    if (cond->getType()->isFloat()) {
        auto zero = new ConstantFloat(0.0f);
        auto fcmp = builder.Create<FCmpInst>(FCmpInst::ONE, cond, zero);
        fcmp->setName(nextValueName());
        return fcmp;
    } else {
        auto zero = new ConstantInt(0);
        auto icmp = builder.Create<ICmpInst>(ICmpInst::NE, cond, zero);
        icmp->setName(nextValueName());
        return icmp;
    }
}

Constant* IRGen::getGlobalInitVal(InitValAST* init, Type* type) {
    if (init->isLeaf()) {
        init->getExpr()->accept(*this);
        Value* computedVal = LastVal;

        if (auto evaluated = evaluateConstantExpr(computedVal)) {
            computedVal = evaluated;
        }

        if (auto val = dyn_cast<Constant>(computedVal)) {
            if (val->getType() == type) {
                return val;
            }
            if (type->isFloat() && val->getType()->isInt()) {
                auto intVal = cast<ConstantInt>(val)->getValue();
                return new ConstantFloat((float)intVal);
            }
            if (type->isInt() && val->getType()->isFloat()) {
                auto floatVal = cast<ConstantFloat>(val)->getValue();
                return new ConstantInt((int)floatVal);
            }
        }

        return new ConstantZero(type);
    }

    if (auto arrTy = dyn_cast<ArrayType>(type)) {
        std::vector<Constant*> elements;
        Type* elemTy = arrTy->getElementType();
        int numElements = arrTy->getNumElements();
        
        const auto& initElements = init->getElements();
        for (int i = 0; i < numElements; ++i) {
            if (i < initElements.size()) {
                elements.push_back(getGlobalInitVal(initElements[i].get(), elemTy));
            } else {
                elements.push_back(new ConstantZero(elemTy));
            }
        }
        return new ConstantArray(arrTy, elements);
    }
    return new ConstantZero(type);
}

Constant* IRGen::evaluateConstantExpr(Value* val) {
    if (!val) return nullptr;

    if (auto c = dyn_cast<Constant>(val)) {
        return c;
    }

    if (auto bin = dyn_cast<BinaryInst>(val)) {
        auto lhs = evaluateConstantExpr(bin->getOperand(0));
        auto rhs = evaluateConstantExpr(bin->getOperand(1));
        if (lhs && rhs) {
            if (auto i1 = dyn_cast<ConstantInt>(lhs)) {
                if (auto i2 = dyn_cast<ConstantInt>(rhs)) {
                    int v1 = i1->getValue();
                    int v2 = i2->getValue();
                    switch (bin->getOpID()) {
                        case Instruction::Add: return new ConstantInt(v1 + v2);
                        case Instruction::Sub: return new ConstantInt(v1 - v2);
                        case Instruction::Mul: return new ConstantInt(v1 * v2);
                        case Instruction::Div: return v2 != 0 ? new ConstantInt(v1 / v2) : nullptr;
                        case Instruction::Mod: return v2 != 0 ? new ConstantInt(v1 % v2) : nullptr;
                        default: return nullptr;
                    }
                }
            }
            else if (auto f1 = dyn_cast<ConstantFloat>(lhs)) {
                if (auto f2 = dyn_cast<ConstantFloat>(rhs)) {
                    float v1 = f1->getValue();
                    float v2 = f2->getValue();
                    switch (bin->getOpID()) {
                        case Instruction::FAdd: return new ConstantFloat(v1 + v2);
                        case Instruction::FSub: return new ConstantFloat(v1 - v2);
                        case Instruction::FMul: return new ConstantFloat(v1 * v2);
                        case Instruction::FDiv: return v2 != 0.0f ? new ConstantFloat(v1 / v2) : nullptr;
                        default: return nullptr;
                    }
                }
            }
        }
    }
    else if (auto castInst = dyn_cast<CastInst>(val)) {
        auto op = evaluateConstantExpr(castInst->getOperand(0));
        if (op) {
            if (castInst->getOpID() == Instruction::SIToFP) {
                if (auto c = dyn_cast<ConstantInt>(op)) {
                    return new ConstantFloat((float)c->getValue());
                }
            } else if (castInst->getOpID() == Instruction::FPToSI) {
                if (auto c = dyn_cast<ConstantFloat>(op)) {
                    return new ConstantInt((int)c->getValue());
                }
            }
        }
    }
    return nullptr;
}

void IRGen::processLocalInit(InitValAST* init, Value* baseAddr, Type* type, std::vector<int>& indices) {
    if (init->isLeaf()) {
        init->getExpr()->accept(*this);
        Value* val = LastVal;

        val = castTo(val, type);
        
        Value* ptr = baseAddr;
        for (int idx : indices) {
            auto gep = builder.Create<GetElementPtrInst>(ptr, new ConstantInt(idx));
            gep->setName(nextValueName());
            ptr = gep;
        }
        
        builder.Create<StoreInst>(val, ptr);
        return;
    }

    if (auto arrTy = dyn_cast<ArrayType>(type)) {
        Type* elemTy = arrTy->getElementType();
        const auto& elems = init->getElements();
        
        for (size_t i = 0; i < elems.size(); ++i) {
            indices.push_back(i);
            processLocalInit(elems[i].get(), baseAddr, elemTy, indices);
            indices.pop_back();
        }

        int size = arrTy->getNumElements();
        for (size_t i = elems.size(); i < size; ++i) {
             indices.push_back(i);

             fillZero(baseAddr, elemTy, indices);
             indices.pop_back();
        }
    }
}

void IRGen::fillZero(Value* baseAddr, Type* type, std::vector<int>& indices) {
    if (type->isInt() || type->isFloat()) {
        Value* ptr = baseAddr;
        for (int idx : indices) {
            auto gep = builder.Create<GetElementPtrInst>(ptr, new ConstantInt(idx));
            gep->setName(nextValueName());
            ptr = gep;
        }
        Constant* zero = type->isFloat() ? (Constant*)new ConstantFloat(0.0f) : (Constant*)new ConstantInt(0);
        builder.Create<StoreInst>(zero, ptr);
    } 
    else if (auto arrTy = dyn_cast<ArrayType>(type)) {
        Type* elemTy = arrTy->getElementType();
        int size = arrTy->getNumElements();
        for (int i = 0; i < size; ++i) {
            indices.push_back(i);
            fillZero(baseAddr, elemTy, indices);
            indices.pop_back();
        }
    }
}

void IRGen::visit(CompUnitAST &node) {
    enterScope();
    for (auto &child : node.getChildren()) {
        child->accept(*this);
    }
    exitScope();
}

void IRGen::visit(FuncCallAST &node) {
    std::string funcName = node.getName();

    // fix
    if (funcName == "starttime") {
        funcName = "_sysy_starttime";
    } else if (funcName == "stoptime") {
        funcName = "_sysy_stoptime";
    }

    Function *callee = TheModule->getFunction(funcName);

    if (!callee) {
        Type* retType = Type::getIntTy(); 
        if (funcName == "putint" || funcName == "putch" || funcName == "putarray" ||
            funcName == "putfarray" || funcName == "putfloat" || funcName == "_sysy_starttime" || funcName == "_sysy_stoptime")
            retType = Type::getVoidTy();
        else if (funcName == "getint" || funcName == "getch" || funcName == "getarray" || funcName == "getfarray")
            retType = Type::getIntTy();
        else if (funcName == "getfloat")
            retType = Type::getFloatTy();
            
        callee = new Function(funcName, retType);
        // For _sysy_starttime/stoptime, they require line number,
        // and only needs to receive one parameter.
        if (funcName == "_sysy_starttime" || funcName == "_sysy_stoptime") {
            callee->addArgument(new Argument(Type::getIntTy(), "lineno", callee, 0));
        }
        TheModule->addFunction(callee);
    }

    std::vector<Value*> args;

    if (funcName == "_sysy_starttime" || funcName == "_sysy_stoptime") {
        args.push_back(new ConstantInt(0));
    } else {
        for (auto &argNode : node.getArgs()) {
            bool oldMode = isLValMode;
            isLValMode = false;
            argNode->accept(*this);
            isLValMode = oldMode;

            args.push_back(LastVal);
        }
    }

    auto call = builder.Create<CallInst>(callee, args);
    if (!callee->getType()->isVoid()) {
        call->setName(nextValueName());
        LastVal = call;
    } else {
        LastVal = nullptr;
    }
}

void IRGen::visit(FuncDefAST &node) {
    Type* retType = Type::getIntTy();
    if (node.getRetType() == "void") retType = Type::getVoidTy();
    else if (node.getRetType() == "float") retType = Type::getFloatTy();

    auto func = new Function(node.getName(), retType);
    TheModule->addFunction(func);
    CurrentFunc = func;
    ValueCounter = 0;
    LabelCounter = 0;

    std::vector<Value*> argAllocas;

    for (size_t i = 0; i < node.getParams().size(); ++i) {
        auto &paramNode = node.getParams()[i];

        Type* baseTy = (paramNode->getType() == "float") ? Type::getFloatTy() : Type::getIntTy();
        Type* argTy = baseTy;

        if (!paramNode->getDims().empty()) {
            std::function<int(ASTNode*)> evalConst = [&](ASTNode* n) -> int {
                if (!n) return 0;
                if (auto num = dynamic_cast<NumberAST*>(n)) return num->getIntVal();
                if (auto lval = dynamic_cast<LValAST*>(n)) {
                    Value* addr = this->lookupVar(lval->getName());
                    if (auto gv = dyn_cast<GlobalVariable>(addr)) {
                        if (auto init = dyn_cast<ConstantInt>(gv->getInit())) return init->getValue();
                    } else if (auto alloca = dyn_cast<AllocaInst>(addr)) {
                        BasicBlock* bb = this->builder.GetInsertPoint();
                        if (bb) {
                            for (auto inst : bb->getInstructions()) {
                                if (auto store = dyn_cast<StoreInst>(inst)) {
                                    if (store->getOperand(1) == alloca) {
                                        if (auto c = dyn_cast<ConstantInt>(store->getOperand(0))) return c->getValue();
                                    }
                                }
                            }
                        }
                    }
                    return 0;
                }
                if (auto bin = dynamic_cast<BinaryExprAST*>(n)) {
                    int lhs = evalConst(bin->getLHS());
                    int rhs = evalConst(bin->getRHS());
                    std::string op = bin->getOp();
                    if (op == "+") return lhs + rhs;
                    if (op == "-") return lhs - rhs;
                    if (op == "*") return lhs * rhs;
                    if (op == "/") return rhs != 0 ? lhs / rhs : 0;
                    if (op == "%") return rhs != 0 ? lhs % rhs : 0;
                    return 0;
                }
                if (auto un = dynamic_cast<UnaryExprAST*>(n)) {
                    int val = evalConst(un->getOperand());
                    if (un->getOp() == "-") return -val;
                    if (un->getOp() == "!") return val == 0 ? 1 : 0;
                    return val;
                }
                return 0;
            };

            const auto &dims = paramNode->getDims();
            for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
                if (it == dims.rend() - 1) continue;
                int size = evalConst(it->get());
                baseTy = new ArrayType(baseTy, size);
            }
            argTy = new PointerType(baseTy);
        }
        std::string argName = "%arg" + std::to_string(i);
        auto arg = new Argument(argTy, argName, func, i);
        func->addArgument(arg);
    }

    BasicBlock *entryBlock = new BasicBlock(newLabelName(), func->getBody());
    builder.SetInsertPoint(entryBlock);

    enterScope();

    const auto &args = func->getArgs();
    for (size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        auto &paramNode = node.getParams()[i];

        auto alloca = builder.Create<AllocaInst>(arg->getType());
        alloca->setName("%" + paramNode->getName() + "_addr");

        builder.Create<StoreInst>(arg, alloca);

        defineVar(paramNode->getName(), alloca);
    }

    if (node.getBody()) node.getBody()->accept(*this);
    
    exitScope();

    BasicBlock *curr = builder.GetInsertPoint();
    if (curr->getInstructions().empty() || !curr->getInstructions().back()->isTerminator()) {
        if (retType->isVoid()) builder.CreateRet(nullptr);
        else builder.CreateRet(new ConstantInt(0));
    }

    CurrentFunc = nullptr;
}

void IRGen::visit(FuncFParamAST &node) {}

void IRGen::visit(BlockAST &node) {
    enterScope();
    for (auto &item : node.getItems()) {
        item->accept(*this);
        // If the current block has ended (e.g. encountering return/break), subsequent code is no longer generated.
        // Prevent multiple terminators from being generated in a BasicBlock.
        if (builder.GetInsertPoint()->getInstructions().size() > 0 &&
            builder.GetInsertPoint()->getInstructions().back()->isTerminator()) {
            break;
        }
    }
    exitScope();
}

void IRGen::visit(VarDeclAST &node) {
    Type* varType = (node.getType() == "float") ? Type::getFloatTy() : Type::getIntTy();
    std::function<int(ASTNode*)> evalConst = [&](ASTNode* n) -> int {
        if (!n) return 0;
        if (auto num = dynamic_cast<NumberAST*>(n)) return num->getIntVal();
        if (auto lval = dynamic_cast<LValAST*>(n)) {
            Value* addr = this->lookupVar(lval->getName());
            if (auto gv = dyn_cast<GlobalVariable>(addr)) {
                if (auto init = dyn_cast<ConstantInt>(gv->getInit())) return init->getValue();
            } else if (auto alloca = dyn_cast<AllocaInst>(addr)) {
                BasicBlock* bb = this->builder.GetInsertPoint();
                if (bb) {
                    for (auto inst : bb->getInstructions()) {
                        if (auto store = dyn_cast<StoreInst>(inst)) {
                            if (store->getOperand(1) == alloca) {
                                if (auto c = dyn_cast<ConstantInt>(store->getOperand(0))) return c->getValue();
                            }
                        }
                    }
                }
            }
            return 0;
        }
        if (auto bin = dynamic_cast<BinaryExprAST*>(n)) {
            int lhs = evalConst(bin->getLHS());
            int rhs = evalConst(bin->getRHS());
            std::string op = bin->getOp();
            if (op == "+") return lhs + rhs;
            if (op == "-") return lhs - rhs;
            if (op == "*") return lhs * rhs;
            if (op == "/") return rhs != 0 ? lhs / rhs : 0;
            if (op == "%") return rhs != 0 ? lhs % rhs : 0;
            return 0;
        }
        if (auto un = dynamic_cast<UnaryExprAST*>(n)) {
            int val = evalConst(un->getOperand());
            if (un->getOp() == "-") return -val;
            if (un->getOp() == "!") return val == 0 ? 1 : 0;
            return val;
        }
        return 0;
    };

    const auto &dims = node.getDims();
    for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
        int size = evalConst(it->get());
        varType = new ArrayType(varType, size);
    }

    if (CurrentFunc == nullptr) {
        Constant *initVal = nullptr;
        if (node.getInit()) {
            initVal = getGlobalInitVal(node.getInit(), varType);
        } else {
            initVal = new ConstantZero(varType);
        }
        
        auto globalVar = new GlobalVariable(node.getName(), varType, initVal);
        TheModule->addGlobalVariable(globalVar);
        defineVar(node.getName(), globalVar);
        return;
    }

    // Alloca hoists to the entryblock 
    BasicBlock* entryBB = CurrentFunc->getEntryBlock();
    auto alloca = new AllocaInst(varType, entryBB);

    auto& instList = entryBB->getInstructions();
    instList.pop_back();

    // Insert in positive order, for the correct layout.
    auto it = instList.begin();
    while (it != instList.end() && (*it)->getOpID() == Instruction::Alloca) {
        ++it;
    }
    instList.insert(it, alloca);

    alloca->setName("%" + node.getName() + "_" + std::to_string(ValueCounter++));
    
    defineVar(node.getName(), alloca);

    if (node.getInit()) {
        std::vector<int> indices;
        processLocalInit(node.getInit(), alloca, varType, indices);
    }
}

void IRGen::visit(BreakStmtAST &node) {
    builder.Create<BreakInst>();
}

void IRGen::visit(ContinueStmtAST &node) {
    builder.Create<ContinueInst>();
}

void IRGen::visit(AssignStmtAST &node) {
    isLValMode = true;
    node.getLVal()->accept(*this);
    isLValMode = false;
    Value *addr = LastVal;

    node.getValue()->accept(*this);
    Value *val = LastVal;

    if (addr && val) {
        Type* targetTy = dyn_cast<PointerType>(addr->getType())->getPointeeType();
        val = castTo(val, targetTy);
        builder.Create<StoreInst>(val, addr);
    }

}

void IRGen::visit(LValAST &node) {
    Value *addr = lookupVar(node.getName());
    if (!addr) {
        LastVal = nullptr;
        return;
    }

    int maxIndices = getDimCount(addr->getType()) - 1;
    // Whether the variable is from the local or from the function parameter.
    bool isParamPointer = false;

    if (auto ptrTy = dyn_cast<PointerType>(addr->getType())) {
        if (ptrTy->getPointeeType()->isPointer()) {
            auto load = builder.Create<LoadInst>(addr);
            load->setName(nextValueName());
            addr = load;
            isParamPointer = true;
        }
    }

    // If the provided index count is less than the array dimension,
    // it implies that it degenerates into a pointer here.
    bool isPartial = node.getIndices().size() < maxIndices;

    for (auto &indexExpr : node.getIndices()) {
        bool oldMode = isLValMode;
        isLValMode = false;
        indexExpr->accept(*this);
        isLValMode = oldMode;
        Value *indexVal = LastVal;

        auto gep = builder.Create<GetElementPtrInst>(addr, indexVal);
        gep->setName(nextValueName());
        addr = gep;
    }

    if (isLValMode) {
        LastVal = addr;
    } else {
        if (isPartial) {
            if (!isParamPointer) {
                auto zero = new ConstantInt(0);
                auto gep = builder.Create<GetElementPtrInst>(addr, zero);
                gep->setName(nextValueName());
                LastVal = gep;
            } else {
                LastVal = addr;
            }
        } else {
            auto load = builder.Create<LoadInst>(addr);
            load->setName(nextValueName());
            LastVal = load;
        }
    }
}

void IRGen::visit(NumberAST &node) {
    if (node.isInt()) {
        LastVal = new ConstantInt(node.getIntVal()); 
    } else {
        LastVal = new ConstantFloat(node.getFloatVal());
    }
}

void IRGen::visit(BinaryExprAST &node) {
    std::string opStr =node.getOp();

    // Short-circuit evaluation.
    if (opStr == "&&" || opStr == "||") {
        node.getLHS()->accept(*this);
        Value *L = LastVal;
        L = toCondition(L);

        BasicBlock* currentBB = builder.GetInsertPoint();
        Region* currentRegion = currentBB->getParent();

        BasicBlock* rhsBB = new BasicBlock(newLabelName(), currentRegion);
        BasicBlock* mergeBB = new BasicBlock(newLabelName(), currentRegion);

        // If it's &&: L is true then calculate RHS, false directly jump merge return 0.
        // If it's ||: L is false then calculate RHS, true directly jump merge return 1.
        if (opStr == "&&") {
            builder.Create<BranchInst>(L, rhsBB, mergeBB);
        } else {
            builder.Create<BranchInst>(L, mergeBB, rhsBB);
        }

        builder.SetInsertPoint(rhsBB);
        node.getRHS()->accept(*this);
        Value* R = LastVal;
        R = toCondition(R);
        // There may also be nested if/while inside new blocks.
        BasicBlock* rhsEndBB = builder.GetInsertPoint(); 
        builder.Create<BranchInst>(mergeBB);

        builder.SetInsertPoint(mergeBB);
        auto phi = builder.Create<PhiInst>(Type::getIntTy());
        phi->setName(nextValueName());

        if (opStr == "&&") {
            phi->addIncoming(new ConstantInt(0), currentBB);
            phi->addIncoming(R, rhsEndBB);
        } else {
            phi->addIncoming(new ConstantInt(1), currentBB);
            phi->addIncoming(R, rhsEndBB);
        }

        LastVal = phi;
        return;
    }

    node.getLHS()->accept(*this);
    Value *L = LastVal;
    node.getRHS()->accept(*this);
    Value *R = LastVal;

    if (!L || !R) return;

    bool isFloat = L->getType()->isFloat() || R->getType()->isFloat();
    Type* targetTy = isFloat ? Type::getFloatTy() : Type::getIntTy();

    L = castTo(L, targetTy);
    R = castTo(R, targetTy);

    Instruction *inst = nullptr;

    if (opStr == ">" || opStr == "<" || opStr == "==" || 
        opStr == ">=" || opStr == "<=" || opStr == "!=") {
        
        if (isFloat) {
            FCmpInst::CmpOp pred = FCmpInst::OEQ;
            if (opStr == ">") pred = FCmpInst::OGT;
            else if (opStr == "<") pred = FCmpInst::OLT;
            else if (opStr == "==") pred = FCmpInst::OEQ;
            else if (opStr == "!=") pred = FCmpInst::ONE;
            else if (opStr == ">=") pred = FCmpInst::OGE;
            else if (opStr == "<=") pred = FCmpInst::OLE;

            inst = builder.Create<FCmpInst>(pred, L, R);
        } else {
            ICmpInst::CmpOp pred = ICmpInst::EQ;
            if (opStr == ">") pred = ICmpInst::SGT;
            else if (opStr == "<") pred = ICmpInst::SLT;
            else if (opStr == "==") pred = ICmpInst::EQ;
            else if (opStr == "!=") pred = ICmpInst::NE;
            else if (opStr == ">=") pred = ICmpInst::SGE;
            else if (opStr == "<=") pred = ICmpInst::SLE;

            inst = builder.Create<ICmpInst>(pred, L, R);
        }
    } else {
        Instruction::OpID op = Instruction::Add;
        if (isFloat) {
            if (opStr == "+") op = Instruction::FAdd;
            else if (opStr == "-") op = Instruction::FSub;
            else if (opStr == "*") op = Instruction::FMul;
            else if (opStr == "/") op = Instruction::FDiv;
        } else {
            if (opStr == "+") op = Instruction::Add;
            else if (opStr == "-") op = Instruction::Sub;
            else if (opStr == "*") op = Instruction::Mul;
            else if (opStr == "/") op = Instruction::Div;
            else if (opStr == "%") op = Instruction::Mod;
        }

        inst = builder.Create<BinaryInst>(op, L, R);
    }

    if (inst) {
        inst->setName(nextValueName());
        LastVal = inst;
    }
}

void IRGen::visit(IfStmtAST &node) {
    node.getCond()->accept(*this);
    Value *cond = LastVal;

    cond = toCondition(cond);

    if (!cond) return;

    auto ifInst = builder.Create<IfInst>(cond);

    {
        BasicBlock *thenBlock = new BasicBlock(newLabelName(), ifInst->getThenRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        
        builder.SetInsertPoint(thenBlock);
        node.getThen()->accept(*this);

        builder.SetInsertPoint(originalBlock);
    }

    if (node.getElse()) {
        ifInst->addElseRegion();
        BasicBlock *elseBlock = new BasicBlock(newLabelName(), ifInst->getElseRegion());
        
        BasicBlock *originalBlock = builder.GetInsertPoint();
        builder.SetInsertPoint(elseBlock);
        node.getElse()->accept(*this);
        builder.SetInsertPoint(originalBlock);
    }
}

void IRGen::visit(WhileStmtAST &node) {
    auto whileInst = builder.Create<WhileInst>();

    {
        BasicBlock *condBlock = new BasicBlock(newLabelName(), whileInst->getCondRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        
        builder.SetInsertPoint(condBlock);
        node.getCond()->accept(*this);

        LastVal = toCondition(LastVal);

        builder.SetInsertPoint(originalBlock);
    }

    {
        BasicBlock *bodyBlock = new BasicBlock(newLabelName(), whileInst->getBodyRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        
        builder.SetInsertPoint(bodyBlock);
        node.getBody()->accept(*this);
        
        builder.SetInsertPoint(originalBlock);
    }
}

void IRGen::visit(ReturnStmtAST &node) {
    if (node.getRetVal()) {
        node.getRetVal()->accept(*this);
        Value *retVal = LastVal;
        retVal =castTo(retVal, CurrentFunc->getType());
        builder.CreateRet(retVal);
    } else {
        if (CurrentFunc->getType()->isVoid()) {
            builder.CreateRet(nullptr);
        } else {
            builder.CreateRet(new ConstantZero(CurrentFunc->getType()));
        }
    }
}

void IRGen::visit(ExprStmtAST &node) {
    if (node.getExpr()) node.getExpr()->accept(*this);
}

void IRGen::visit(UnaryExprAST &node) {
    node.getOperand()->accept(*this);
    Value *operand = LastVal;
    if (node.getOp() == "-") {
        Instruction* inst = nullptr;
        if (operand->getType()->isFloat()) {
            auto zero = new ConstantFloat(0.0f);
            inst = builder.Create<BinaryInst>(Instruction::FSub, zero, operand);
        } else {
            auto zero = new ConstantInt(0);
            inst = builder.Create<BinaryInst>(Instruction::Sub, zero, operand);
        }
        inst->setName(nextValueName());
        LastVal = inst;
    }
    else if (node.getOp() == "!") {
        operand = toCondition(operand);
        auto zero = new ConstantInt(0);
        auto inst = builder.Create<ICmpInst>(ICmpInst::EQ, operand, zero);
        inst->setName(nextValueName());
        LastVal = inst;
    }
    else if (node.getOp() == "+") {} // Just keep LastVal as operand.
}
