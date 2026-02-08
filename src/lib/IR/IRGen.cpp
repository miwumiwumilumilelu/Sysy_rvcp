#include "IR/IRGen.h"
#include "IR/Type.h"
#include "IR/Module.h"
#include <iostream>

using namespace sysy;

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

Constant* IRGen::getGlobalInitVal(InitValAST* init, Type* type) {
    if (init->isLeaf()) {
        init->getExpr()->accept(*this);
        if (auto constInt = dyn_cast<ConstantInt>(LastVal)) {
            return constInt;
        }
        return new ConstantInt(0); 
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
                // 填充 0
                elements.push_back(new ConstantZero(elemTy));
            }
        }
        return new ConstantArray(arrTy, elements);
    }
    return new ConstantZero(type);
}

void IRGen::processLocalInit(InitValAST* init, Value* baseAddr, Type* type, std::vector<int>& indices) {
    if (init->isLeaf()) {
        init->getExpr()->accept(*this);
        Value* val = LastVal;
        
        Value* ptr = baseAddr;
        for (int idx : indices) {
            auto gep = builder.Create<GetElementPtrInst>(ptr, new ConstantInt(idx));
            gep->setName(newTempName());
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
    if (type->isInt()) {
        Value* ptr = baseAddr;
        for (int idx : indices) {
            auto gep = builder.Create<GetElementPtrInst>(ptr, new ConstantInt(idx));
            gep->setName(newTempName());
            ptr = gep;
        }
        builder.Create<StoreInst>(new ConstantInt(0), ptr);
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
    Function *callee = TheModule->getFunction(funcName);

    if (!callee) {
        Type* retType = Type::getIntTy(); 
        if (funcName == "putint" || funcName == "putch" || funcName == "putarray") 
            retType = Type::getVoidTy();
        else if (funcName == "getint" || funcName == "getch")
            retType = Type::getIntTy();
        else if (funcName == "getarray" || funcName == "getfarray")
            retType = Type::getIntTy();
            
        callee = new Function(funcName, retType);
        TheModule->addFunction(callee);
    }

    std::vector<Value*> args;
    for (auto &argNode : node.getArgs()) {
        if (dynamic_cast<LValAST*>(argNode.get())) {
            isLValMode = false;
            argNode->accept(*this);
            Value *val = LastVal;

            if (val->getType()->isPointer()) {
                Type* pointee = dyn_cast<PointerType>(val->getType())->getPointeeType();
                if (pointee->isArray()) {
                    auto zero = new ConstantInt(0);
                    auto gep = builder.Create<GetElementPtrInst>(val, zero);

                    gep->setName(newTempName());
                    val = gep;
                }
            }
            args.push_back(val);
        } else {
            argNode->accept(*this);
            args.push_back(LastVal);
        }
    }

    auto call = builder.Create<CallInst>(callee, args);
    if (!callee->getType()->isVoid()) {
        call->setName(newTempName());
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
    TempCounter = 0;

    std::vector<Value*> argAllocas;

    for (size_t i = 0; i < node.getParams().size(); ++i) {
        auto &paramNode = node.getParams()[i];
        Type* argTy = Type::getIntTy();

        if (paramNode->getDims().empty()) {
            argTy = Type::getIntTy();            
        } else {
            Type* baseTy = Type::getIntTy();
            const auto &dims = paramNode->getDims();
            for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
                if (it == dims.rend() - 1) continue;
                int size = 0;
                if (*it) {
                    if (auto num = dynamic_cast<NumberAST*>(it->get())) {
                        size = num->getIntVal();
                    }
                }
                baseTy = new ArrayType(baseTy, size);
            }
            argTy = new PointerType(baseTy);
        }
        std::string argName = "%arg" + std::to_string(i);
        auto arg = new Argument(argTy, argName, func, i);
        func->addArgument(arg);
    }

    BasicBlock *entryBlock = new BasicBlock("entry", func->getBody());
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
    Type* varType = Type::getIntTy();
    const auto &dims = node.getDims();
    for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
        int size = 0;
        if (auto num = dynamic_cast<NumberAST*>(it->get())) {
            size = num->getIntVal();
        }
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

    auto alloca = builder.Create<AllocaInst>(varType);
    alloca->setName("%" + node.getName() + "_" + std::to_string(TempCounter++));
    
    defineVar(node.getName(), alloca);

    if (node.getInit()) {
        std::vector<int> indices;
        processLocalInit(node.getInit(), alloca, varType, indices);
    }
}

void IRGen::visit(AssignStmtAST &node) {
    isLValMode = true;
    node.getLVal()->accept(*this);
    isLValMode = false;
    Value *addr = LastVal;

    node.getValue()->accept(*this);
    Value *val = LastVal;

    if (addr && val) {
        builder.Create<StoreInst>(val, addr);
    }
}

void IRGen::visit(LValAST &node) {
    Value *addr = lookupVar(node.getName());
    if (!addr) {
        LastVal = nullptr;
        return;
    }

    if (auto ptrTy = dyn_cast<PointerType>(addr->getType())) {
        if (ptrTy->getPointeeType()->isPointer()) {
            auto load = builder.Create<LoadInst>(addr);
            load->setName(newTempName());
            addr = load;
        }
    }

    for (auto &indexExpr : node.getIndices()) {
        indexExpr->accept(*this);
        Value *indexVal = LastVal;

        auto gep = builder.Create<GetElementPtrInst>(addr, indexVal);
        gep->setName(newTempName());
        addr = gep;
    }

    if (isLValMode) {
        LastVal = addr;
    } else {
        if (addr->getType()->isPointer() && dyn_cast<PointerType>(addr->getType())->getPointeeType()->isArray()) {
            LastVal = addr;
        } else {
            auto load = builder.Create<LoadInst>(addr);
            load->setName(newTempName());
            LastVal = load;
        }
    }
}

void IRGen::visit(NumberAST &node) {
    if (node.isInt()) {
        LastVal = new ConstantInt(node.getIntVal()); 
    } else {
        LastVal = new ConstantInt((int)node.getFloatVal());
    }
}

void IRGen::visit(BinaryExprAST &node) {
    node.getLHS()->accept(*this);
    Value *L = LastVal;
    node.getRHS()->accept(*this);
    Value *R = LastVal;

    if (!L || !R) return;

    std::string opStr = node.getOp();
    Instruction *inst = nullptr;

    if (opStr == ">" || opStr == "<" || opStr == "==" || 
        opStr == ">=" || opStr == "<=" || opStr == "!=") {
        
        ICmpInst::CmpOp pred = ICmpInst::EQ;
        if (opStr == ">") pred = ICmpInst::SGT;
        else if (opStr == "<") pred = ICmpInst::SLT;
        else if (opStr == "==") pred = ICmpInst::EQ;
        else if (opStr == "!=") pred = ICmpInst::NE;
        else if (opStr == ">=") pred = ICmpInst::SGE;
        else if (opStr == "<=") pred = ICmpInst::SLE;

        inst = builder.Create<ICmpInst>(pred, L, R);
    } else {
        Instruction::OpID op = Instruction::Add;
        if (opStr == "+") op = Instruction::Add;
        else if (opStr == "-") op = Instruction::Sub;
        else if (opStr == "*") op = Instruction::Mul;
        else if (opStr == "/") op = Instruction::Div;

        inst = builder.Create<BinaryInst>(op, L, R);
    }

    if (inst) {
        inst->setName(newTempName());
        LastVal = inst;
    }
}

void IRGen::visit(IfStmtAST &node) {
    node.getCond()->accept(*this);
    Value *cond = LastVal;

    auto ifInst = builder.Create<IfInst>(cond);

    {
        BasicBlock *thenBlock = new BasicBlock("then", ifInst->getThenRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        
        builder.SetInsertPoint(thenBlock);
        node.getThen()->accept(*this);

        builder.SetInsertPoint(originalBlock);
    }

    if (node.getElse()) {
        ifInst->addElseRegion();
        BasicBlock *elseBlock = new BasicBlock("else", ifInst->getElseRegion());
        
        BasicBlock *originalBlock = builder.GetInsertPoint();
        builder.SetInsertPoint(elseBlock);
        node.getElse()->accept(*this);
        builder.SetInsertPoint(originalBlock);
    }
}

void IRGen::visit(WhileStmtAST &node) {
    auto whileInst = builder.Create<WhileInst>();

    {
        BasicBlock *condBlock = new BasicBlock("cond", whileInst->getCondRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        
        builder.SetInsertPoint(condBlock);
        node.getCond()->accept(*this);
// Note: Cond Region should "return" the condition value in some way
// Here we temporarily assume LastVal is the condition, and LowerPass will handle it later
// For convenience in Flatten, we can insert a special Yield instruction here or leave it untreated
// As long as the AST traversal generates icmp or similar instructions within condBlock

// need to store or mark LastVal (i.e., the result of the condition)
// In advanced IR, we typically 约定 the result of the last computation instruction in the Cond Region is the condition
        
        builder.SetInsertPoint(originalBlock);
    }

    {
        BasicBlock *bodyBlock = new BasicBlock("body", whileInst->getBodyRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        
        builder.SetInsertPoint(bodyBlock);
        node.getBody()->accept(*this);
        
        builder.SetInsertPoint(originalBlock);
    }
}

void IRGen::visit(ReturnStmtAST &node) {
    Value *retVal = nullptr;
    if (node.getRetVal()) {
        node.getRetVal()->accept(*this);
        retVal = LastVal;
        builder.CreateRet(retVal);
    } else {
        builder.CreateRet(new ConstantInt(0));
    }
}

void IRGen::visit(ExprStmtAST &node) {
    if (node.getExpr()) node.getExpr()->accept(*this);
}

void IRGen::visit(UnaryExprAST &node) {
    if (node.getOp() == "-") {
        node.getOperand()->accept(*this);
        Value *operand = LastVal;
        auto zero = new ConstantInt(0);
        auto inst = builder.Create<BinaryInst>(Instruction::Sub, zero, operand);
        inst->setName(newTempName());
        LastVal = inst;
    }
}
