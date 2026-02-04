#ifndef IRGEN_H
#define IRGEN_H

#include "AST/ASTVisitor.h"
#include "IR/Module.h"
#include "IR/IRBuilder.h"
#include <map>
#include <vector>
#include <string>
#include <memory>

namespace sysy {

class IRGen : public ASTVisitor {
public:
    IRGen();

    std::unique_ptr<Module> getModule() { return std::move(TheModule); }

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

private:
    std::unique_ptr<Module> TheModule;
    IRBuilder builder;
    
    Function *CurrentFunc = nullptr;
    Value *LastVal = nullptr;

    // Symbol stack: Variable name -> Value* in IR.
    // (usually AllocaInst* address)
    std::vector<std::map<std::string, Value*>> Scopes;

    int TempCounter = 0;
    int LabelCounter = 0;

    void enterScope() { Scopes.emplace_back(); }
    void exitScope() { Scopes.pop_back(); }
    void defineVar(const std::string &name, Value *val);
    Value* lookupVar(const std::string &name);

    std::string newTempName() { return "%t" + std::to_string(TempCounter++); }
    std::string newLabelName() { return "L" + std::to_string(LabelCounter++); }
};

} 
#endif