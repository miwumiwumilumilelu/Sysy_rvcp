#include "IR/IRGen.h"
#include "IR/Type.h"
#include <iostream>

using namespace sysy;

IRGen::IRGen() {
    TheModule = std::make_unique<Module>();
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

void IRGen::visit(CompUnitAST &node) {
    enterScope();
    for (auto &child : node.getChildren()) {
        child->accept(*this);
    }
    exitScope();
}

void IRGen::visit(FuncDefAST &node) {
    auto func = new Function(node.getName(), Type::getIntTy());
    TheModule->addFunction(func);
    
    CurrentFunc = func;
    TempCounter = 0;
    LabelCounter = 0;

    BasicBlock *entryBlock = new BasicBlock("entry", func);
    builder.SetInsertPoint(entryBlock);

    enterScope();
    if (node.getBody()) node.getBody()->accept(*this); 
    exitScope();

    BasicBlock *curr = builder.GetInsertPoint();
    if (curr->getInstructions().empty() || 
        !curr->getInstructions().back()->isTerminator()) {
        builder.CreateRet(new ConstantInt(0));
    }
}

void IRGen::visit(BlockAST &node) {
    enterScope();
    for (auto &item : node.getItems()) {
        item->accept(*this);
    }
    exitScope();
}

void IRGen::visit(VarDeclAST &node) {
    auto alloca = builder.Create<AllocaInst>(Type::getIntTy());
    alloca->setName("%" + node.getName() + "_" + std::to_string(TempCounter++));
    
    defineVar(node.getName(), alloca);

    if (node.getInit()) {
        node.getInit()->accept(*this);
        if (LastVal) {
            builder.Create<StoreInst>(LastVal, alloca);
        }
    }
}

void IRGen::visit(AssignStmtAST &node) {
    node.getValue()->accept(*this);
    Value *val = LastVal;

    Value *addr = lookupVar(node.getLVal()->getName());
    if (addr && val) {
        builder.Create<StoreInst>(val, addr);
    }
}

void IRGen::visit(LValAST &node) {
    Value *addr = lookupVar(node.getName());
    if (addr) {
        auto load = builder.Create<LoadInst>(addr);
        load->setName(newTempName());
        LastVal = load;
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

    auto thenBB = new BasicBlock(newLabelName(), CurrentFunc);
    auto elseBB = node.getElse() ? new BasicBlock(newLabelName(), CurrentFunc) : nullptr;
    auto mergeBB = new BasicBlock(newLabelName(), CurrentFunc);

    builder.Create<BranchInst>(cond, thenBB, elseBB ? elseBB : mergeBB);

    builder.SetInsertPoint(thenBB);
    node.getThen()->accept(*this);
    
    if (builder.GetInsertPoint()->getInstructions().empty() || 
        !builder.GetInsertPoint()->getInstructions().back()->isTerminator()) {
        builder.CreateBr(mergeBB);
    }

    if (elseBB) {
        builder.SetInsertPoint(elseBB);
        node.getElse()->accept(*this);
        if (builder.GetInsertPoint()->getInstructions().empty() || 
            !builder.GetInsertPoint()->getInstructions().back()->isTerminator()) {
            builder.CreateBr(mergeBB);
        }
    }

    builder.SetInsertPoint(mergeBB);
}

void IRGen::visit(WhileStmtAST &node) {
    auto condBB = new BasicBlock(newLabelName(), CurrentFunc);
    auto bodyBB = new BasicBlock(newLabelName(), CurrentFunc);
    auto endBB  = new BasicBlock(newLabelName(), CurrentFunc);

    builder.CreateBr(condBB);

    // Cond
    builder.SetInsertPoint(condBB);
    node.getCond()->accept(*this);
    builder.Create<BranchInst>(LastVal, bodyBB, endBB);

    // Body
    builder.SetInsertPoint(bodyBB);
    node.getBody()->accept(*this);
    builder.CreateBr(condBB); // 循环回去

    // End
    builder.SetInsertPoint(endBB);
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