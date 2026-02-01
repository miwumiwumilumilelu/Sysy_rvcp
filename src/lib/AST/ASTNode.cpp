#include "AST/ASTNode.h"
#include "AST/ASTVisitor.h"
using namespace sysy;

void NumberAST::accept(ASTVisitor &v) { v.visit(*this); }
void LValAST::accept(ASTVisitor &v) { v.visit(*this); }
void BinaryExprAST::accept(ASTVisitor &v) { v.visit(*this); }
void UnaryExprAST::accept(ASTVisitor &v) { v.visit(*this); }
void ReturnStmtAST::accept(ASTVisitor &v) { v.visit(*this); }
void AssignStmtAST::accept(ASTVisitor &v) { v.visit(*this); }
void IfStmtAST::accept(ASTVisitor &v) { v.visit(*this); }
void WhileStmtAST::accept(ASTVisitor &v) { v.visit(*this); }
void ExprStmtAST::accept(ASTVisitor &v) { v.visit(*this); }
void BlockAST::accept(ASTVisitor &v) { v.visit(*this); }
void VarDeclAST::accept(ASTVisitor &v) { v.visit(*this); }
void FuncDefAST::accept(ASTVisitor &v) { v.visit(*this); }
void CompUnitAST::accept(ASTVisitor &v) { v.visit(*this); }

void NumberAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "NumberAST: " 
        << (Kind == IntKind ? std::to_string(IntVal) : std::to_string(FloatVal)) << std::endl;
}

void LValAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "LValAST: " << Name << std::endl;
}

void BinaryExprAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "BinaryExprAST: " << Op << std::endl;
    if (LHS) LHS->dump(indent + 2);
    if (RHS) RHS->dump(indent + 2);
}

void UnaryExprAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "UnaryExprAST: " << Op << std::endl;
    if (Operand) Operand->dump(indent + 2);
}

void VarDeclAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "VarDeclAST: " << Type << " " << Name;
    if (InitExpr) {
        std::cout << " =" << std::endl;
        InitExpr->dump(indent + 2);
    } else {
        std::cout << std::endl;
    }
}

void ReturnStmtAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "ReturnStmtAST" << std::endl;
    if (RetVal) RetVal->dump(indent + 2);
}

void AssignStmtAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "AssignStmtAST" << std::endl;
    if (LVal) LVal->dump(indent + 2);
    if (Value) Value->dump(indent + 2);
}

void IfStmtAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "IfStmtAST" << std::endl;
    std::cout << std::string(indent+2, ' ') << "Cond:" << std::endl;
    Cond->dump(indent + 4);
    std::cout << std::string(indent+2, ' ') << "Then:" << std::endl;
    Then->dump(indent + 4);
    if (Else) {
        std::cout << std::string(indent+2, ' ') << "Else:" << std::endl;
        Else->dump(indent + 4);
    }
}

void WhileStmtAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "WhileStmtAST" << std::endl;
    Cond->dump(indent + 2);
    Body->dump(indent + 2);
}

void ExprStmtAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "ExprStmtAST" << std::endl;
    if (Expr) Expr->dump(indent + 2);
}

void BlockAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "BlockAST" << std::endl;
    for (auto &item : Items) item->dump(indent + 2);
}

void FuncDefAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "FuncDefAST: " << Name 
              << " [" << RetType << "]" << std::endl;
    // if (Params) Params->dump(indent + 2);
    if (Body) Body->dump(indent + 2);
}

void CompUnitAST::dump(int indent) const {
    std::cout << "CompUnitAST:" << std::endl;
    for (const auto &child : Children) {
        if (child) child->dump(indent + 2);
    }
}