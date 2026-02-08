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
void FuncCallAST::accept(ASTVisitor &v) { v.visit(*this); }
void FuncFParamAST::accept(ASTVisitor &v) {v.visit(*this); }

void FuncCallAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "FuncCallAST: " << Name << std::endl;
    for (const auto &arg : Args) {
        arg->dump(indent + 2);
    }
}

void NumberAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "NumberAST: " 
        << (Kind == IntKind ? std::to_string(IntVal) : std::to_string(FloatVal)) << std::endl;
}

void LValAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "LValAST: " << Name;
    for (const auto &idx : Indices) {
        std::cout << "[";
        idx->dump(0);
        std::cout << "]";
    }
    std::cout << std::endl;
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

void InitValAST::dump(int indent) const {
    std::string pad(indent, ' ');
    if (IsLeaf) {
        std::cout << pad << "InitValAST(Leaf):" << std::endl;
        Expr->dump(indent + 2);
    } else {
        std::cout << pad << "InitValAST(List):" << std::endl;
        for (const auto &elem : Elements) {
            elem->dump(indent + 2);
        }
    }
}

void VarDeclAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "VarDeclAST: " << Type << " " << Name << std::endl;
    for (const auto &dim : Dims) {
        dim->dump(indent + 2);
    }
    if (Init) {
        Init->dump(indent + 2);
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

void FuncFParamAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "Param: " << Type << " " << Name;
    for (const auto &dim : Dims) {
        std::cout << "[";
        if(dim) dim->dump(0);
        std::cout << "]";
    }
    std::cout << std::endl;
}

void FuncDefAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "FuncDefAST: " << RetType << " " << Name << std::endl;
    for (const auto &param : Params) {
        param->dump(indent + 2);
    }
    if (Body) Body->dump(indent + 2);
}

void CompUnitAST::dump(int indent) const {
    std::cout << "CompUnitAST:" << std::endl;
    for (const auto &child : Children) {
        if (child) child->dump(indent + 2);
    }
}