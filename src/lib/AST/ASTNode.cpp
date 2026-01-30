#include "AST/ASTNode.h"
using namespace sysy;

void NumberAST::dump(int indent) const {
    std::string space(indent, ' ');
    if (Kind == IntKind) {
        std::cout << space << "NumberAST: " << IntVal << " (int)" << std::endl;
    } else {
        std::cout << space << "NumberAST: " << FloatVal << " (float)" << std::endl;
    }
}

void BinaryExprAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "BinaryExprAST: " << Op << std::endl;
    LHS->dump(indent + 2);
    RHS->dump(indent + 2);
}

void ReturnStmtAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "ReturnStmtAST" << std::endl;
    if (RetVal) RetVal->dump(indent + 2);
}

void BlockAST::dump(int indent) const {
    std::cout << std::string(indent, ' ') << "BlockAST" << std::endl;
    for (auto &item : Items) item->dump(indent + 2);
}

void FuncDefAST::dump(int indent) const {
    std::string space(indent, ' ');
    std::cout << space << "FuncDefAST: " << Name 
              << " [ReturnType: " << RetType << "]" << std::endl;
    // if (Params) Params->dump(indent + 2);
    if (Body) Body->dump(indent + 2);
}

void CompUnitAST::dump(int indent) const {
    std::cout << "CompUnitAST:" << std::endl;
    for (const auto &child : Children) {
        child->dump(indent + 2);
    }
}