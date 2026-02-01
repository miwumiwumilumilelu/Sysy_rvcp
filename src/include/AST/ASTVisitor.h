#ifndef ASTVISITOR_H
#define ASTVISITOR_H

#include "AST/ASTNode.h"

namespace sysy {
class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit(CompUnitAST &node) = 0;
    virtual void visit(FuncDefAST &node) = 0;
    virtual void visit(BlockAST &node) = 0;
    virtual void visit(VarDeclAST &node) = 0;
    virtual void visit(IfStmtAST &node) = 0;
    virtual void visit(WhileStmtAST &node) = 0;
    virtual void visit(ReturnStmtAST &node) = 0;
    virtual void visit(AssignStmtAST &node) = 0;
    virtual void visit(ExprStmtAST &node) = 0;
    virtual void visit(BinaryExprAST &node) = 0;
    virtual void visit(UnaryExprAST &node) = 0;
    virtual void visit(LValAST &node) = 0;
    virtual void visit(NumberAST &node) = 0;   
}; 

}

#endif