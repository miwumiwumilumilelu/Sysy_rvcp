#include "Semant/Semant.h"

using namespace sysy;

bool Semant::defineSymbol(const std::string &name, const std::string &type) {
    auto &currScope = Scopes.back();
    if (currScope.find(name) != currScope.end()) {
        std::cerr << "Semantic Error: Redefinition of variable '" << name << "'" << std::endl;
        return false;
    }
    currScope[name] = type;
    std::cout<< "Debug: Defined '" << name << "' type: " << type << std::endl;
    return true;
}

bool Semant::checkSymbol(const std::string &name) {
    for (auto scopeIt = Scopes.rbegin(); scopeIt != Scopes.rend(); ++scopeIt) {
        if (scopeIt->find(name) != scopeIt->end()) {
            return true;
        }
    }
    std::cerr << "Semantic Error: Undeclared variable '" << name << "'" << std::endl;
    return false;
}

void Semant::visit(CompUnitAST &node) {
    for (auto &child : node.getChildren()) {
        child->accept(*this);
    }
}

void Semant::visit(FuncDefAST &node) {
    defineSymbol(node.getName(), "func");
    if (node.getBody()) node.getBody()->accept(*this);
}

void Semant::visit(BlockAST &node) {
    enterScope();
    for (auto &item : node.getItems()) {
        item->accept(*this);
    }
    exitScope();
}

void Semant::visit(VarDeclAST &node) {
    if (node.getInit()) {
        node.getInit()->accept(*this);
    }
    defineSymbol(node.getName(), node.getType());
}

void Semant::visit(AssignStmtAST &node) {
    node.getLVal()->accept(*this);
    node.getValue()->accept(*this);
}

void Semant::visit(LValAST &node) {
    checkSymbol(node.getName());
}

void Semant::visit(IfStmtAST &node) {
    if (node.getCond()) node.getCond()->accept(*this);
    if (node.getThen()) node.getThen()->accept(*this);
    if (node.getElse()) node.getElse()->accept(*this);
}

void Semant::visit(WhileStmtAST &node) {
    if (node.getCond()) node.getCond()->accept(*this);
    if (node.getBody()) node.getBody()->accept(*this);
}

void Semant::visit(ReturnStmtAST &node) {
    if (node.getRetVal()) node.getRetVal()->accept(*this);
}

void Semant::visit(ExprStmtAST &node) {
    if (node.getExpr()) node.getExpr()->accept(*this);
}

void Semant::visit(BinaryExprAST &node) {
    if (node.getLHS()) node.getLHS()->accept(*this);
    if (node.getRHS()) node.getRHS()->accept(*this);
}

void Semant::visit(UnaryExprAST &node) {
    if (node.getOperand()) node.getOperand()->accept(*this);
}

void Semant::visit(NumberAST &node) {
    // The numbers do not need to be checked.
}