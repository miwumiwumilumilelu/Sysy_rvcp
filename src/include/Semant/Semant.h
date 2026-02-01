#ifndef SEMANT_H
#define SEMANT_H

#include "AST/ASTVisitor.h"
#include <vector>
#include <map>
#include <iostream>
#include <string>

namespace sysy {

class Semant : public ASTVisitor {
    // Maintain a Scope stack, each of which is a map (variable name -> type).
    std::vector<std::map<std::string, std::string>> Scopes;
public:
    Semant() {
        enterScope(); // Global scope
    }

    void enterScope() { Scopes.emplace_back(); }
    void exitScope() {
        if (!Scopes.empty()) {
            Scopes.pop_back();
        }
    }
    
    bool defineSymbol(const std::string &name, const std::string &type);

    bool checkSymbol(const std::string &name);

    void visit(CompUnitAST &node) override;
    void visit(FuncDefAST &node) override;
    void visit(BlockAST &node) override;
    void visit(VarDeclAST &node) override;
    void visit(IfStmtAST &node) override;
    void visit(WhileStmtAST &node) override;
    void visit(ReturnStmtAST &node) override;
    void visit(AssignStmtAST &node) override;
    void visit(ExprStmtAST &node) override;
    void visit(BinaryExprAST &node) override;
    void visit(UnaryExprAST &node) override;
    void visit(LValAST &node) override;
    void visit(NumberAST &node) override;
};

}

#endif