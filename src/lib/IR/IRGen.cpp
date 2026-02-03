#include "IR/IRGen.h"
#include "IR/Type.h"
#include "IR/Instruction.h"
#include "IR/Module.h" 
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
    enterScope(); // Global scope
    for (auto &child : node.getChildren()) {
        child->accept(*this);
    }
    exitScope();
}

void IRGen::visit(FuncDefAST &node) {
    // Assume temporarily that all return values are int.
    auto func = new Function(node.getName(), Type::getIntTy());
    TheModule->addFunction(func);
    
    CurrentFunc = func;
    TempCounter = 0;
    LabelCounter = 0;

    // Create Entry Block.
    CurrentBlock = new BasicBlock("entry", func);

    enterScope();
    if (node.getBody()) node.getBody()->accept(*this); 
    exitScope();

    // If there is no terminator (ret/br) at the end of the block, add a ret 0.
    if (CurrentBlock->getInstructions().empty() || 
        !CurrentBlock->getInstructions().back()->isTerminator()) {
        new ReturnInst(new ConstantInt(0), CurrentBlock);
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
    auto alloca = new AllocaInst(Type::getIntTy(), CurrentBlock);
    alloca->setName("%" + node.getName() + "_" + std::to_string(TempCounter++));
    
    defineVar(node.getName(), alloca);

    if (node.getInit()) {
        node.getInit()->accept(*this);
        if (LastVal) {
            new StoreInst(LastVal, alloca, CurrentBlock);
        }
    }
}

void IRGen::visit(AssignStmtAST &node) {
    node.getValue()->accept(*this);
    Value *val = LastVal;

    Value *addr = lookupVar(node.getLVal()->getName());
    if (addr && val) {
        new StoreInst(val, addr, CurrentBlock);
    }
}

void IRGen::visit(LValAST &node) {
    Value *addr = lookupVar(node.getName());
    if (addr) {
        auto load = new LoadInst(addr, CurrentBlock);
        load->setName(newTempName());
        LastVal = load;
    }
}

void IRGen::visit(NumberAST &node) {
    if (node.isInt()) {
        LastVal = new ConstantInt(node.getIntVal()); 
    } else {
        // Floating point is not supported at the moment, or ConstantFloat is extended in the future.
        // LastVal = new ConstantFloat(node.getFloatVal());
        LastVal = new ConstantInt((int)node.getFloatVal());
    }
}

void IRGen::visit(BinaryExprAST &node) {
    node.getLHS()->accept(*this);
    Value *L = LastVal;
    node.getRHS()->accept(*this);
    Value *R = LastVal;

    if (!L || !R) return;

    Instruction::OpID op;
    std::string opStr = node.getOp();
    
    if (opStr == "+") op = Instruction::Add;
    else if (opStr == "-") op = Instruction::Sub;
    else if (opStr == "*") op = Instruction::Mul;
    else if (opStr == "/") op = Instruction::Div;
    else if (opStr == ">" || opStr == "<" || opStr == "==" || 
             opStr == ">=" || opStr == "<=" || opStr == "!=") op = Instruction::ICmp;
    else op = Instruction::Add; 

    if (op == Instruction::ICmp) {
        ICmpInst::CmpOp pred = ICmpInst::EQ;
        if (opStr == ">") pred = ICmpInst::SGT;
        else if (opStr == "<") pred = ICmpInst::SLT;
        else if (opStr == "==") pred = ICmpInst::EQ;
        else if (opStr == "!=") pred = ICmpInst::NE;
        else if (opStr == ">=") pred = ICmpInst::SGE;
        else if (opStr == "<=") pred = ICmpInst::SLE;

        auto inst = new ICmpInst(pred, L, R, CurrentBlock);
        inst->setName(newTempName());
        LastVal = inst;
    } else {
        auto inst = new BinaryInst(op, L, R, CurrentBlock);
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

    // Br cond, Then, Else(or Merge)
    new BranchInst(cond, thenBB, elseBB ? elseBB : mergeBB, CurrentBlock);

    // Then Block
    CurrentBlock = thenBB;
    node.getThen()->accept(*this);
    if (CurrentBlock->getInstructions().empty() || !CurrentBlock->getInstructions().back()->isTerminator())
        new BranchInst(mergeBB, CurrentBlock);

    // Else Block
    if (elseBB) {
        CurrentBlock = elseBB;
        node.getElse()->accept(*this);
        if (CurrentBlock->getInstructions().empty() || !CurrentBlock->getInstructions().back()->isTerminator())
            new BranchInst(mergeBB, CurrentBlock);
    }

    // Merge Block
    CurrentBlock = mergeBB;
}

void IRGen::visit(WhileStmtAST &node) {
    auto condBB = new BasicBlock(newLabelName(), CurrentFunc);
    auto bodyBB = new BasicBlock(newLabelName(), CurrentFunc);
    auto endBB  = new BasicBlock(newLabelName(), CurrentFunc);

    new BranchInst(condBB, CurrentBlock);

    // Cond
    CurrentBlock = condBB;
    node.getCond()->accept(*this);
    new BranchInst(LastVal, bodyBB, endBB, CurrentBlock);

    // Body
    CurrentBlock = bodyBB;
    node.getBody()->accept(*this);
    new BranchInst(condBB, CurrentBlock);

    // End
    CurrentBlock = endBB;
}

void IRGen::visit(ReturnStmtAST &node) {
    Value *retVal = nullptr;
    if (node.getRetVal()) {
        node.getRetVal()->accept(*this);
        retVal = LastVal;
    } else {
        retVal = new ConstantInt(0); 
    }
    new ReturnInst(retVal, CurrentBlock);
}

void IRGen::visit(ExprStmtAST &node) {
    if (node.getExpr()) node.getExpr()->accept(*this);
}

void IRGen::visit(UnaryExprAST &node) {
    if (node.getOp() == "-") {
        node.getOperand()->accept(*this);
        Value *operand = LastVal;
        auto zero = new ConstantInt(0);
        auto inst = new BinaryInst(Instruction::Sub, zero, operand, CurrentBlock);
        inst->setName(newTempName());
        LastVal = inst;
    }
}