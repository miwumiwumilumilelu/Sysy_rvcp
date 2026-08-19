#ifndef IRGEN_H
#define IRGEN_H

#include "../AST/ASTVisitor.h"
#include "Module.h"
#include "IRBuilder.h"
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
    void visit(FuncCallAST &node) override;
    void visit(FuncDefAST &node) override;
    void visit(BlockAST &node) override;
    void visit(VarDeclAST &node) override;
    void visit(BreakStmtAST &node) override;
    void visit(ContinueStmtAST &node) override;
    void visit(IfStmtAST &node) override;
    void visit(WhileStmtAST &node) override;
    void visit(ReturnStmtAST &node) override;
    void visit(AssignStmtAST &node) override;
    void visit(ExprStmtAST &node) override;
    void visit(BinaryExprAST &node) override;
    void visit(UnaryExprAST &node) override;
    void visit(LValAST &node) override;
    void visit(NumberAST &node) override;
    void visit(FuncFParamAST &node) override;

private:
    std::unique_ptr<Module> TheModule;
    IRBuilder builder;

    struct TensorInfo {
        Type* ElementType = nullptr;
        std::vector<int> Dims;
        bool isParameter = false;
        bool isTensor() const {
            return ElementType != nullptr;
        }
    };
    
    Function *CurrentFunc = nullptr;
    Value *LastVal = nullptr;

    // Distinguish whether an address or a value is obtained when accessing an LValAST.
    bool isLValMode = false;

    Value* castTo(Value* val, Type* targetTy);

    // Enforce conversion of any value to a conditional judgment.
    Value* toCondition(Value* cond);

    void defineTensor(const std::string &name, TensorInfo info);
    TensorInfo* lookupTensor(const std::string &name);
    
    Constant* getGlobalInitVal(InitValAST* init, Type* type);
    void processLocalInit(InitValAST* init, Value* baseAddr, Type* type, std::vector<int>& indices);

    void fillZero(Value *baseAddr, Type *type, std::vector<int> &indices);

    int evaluateDim(ASTNode* node);
    TensorInfo getTensorExprInfo (ExprAST *expr);
    Value* getTensorElementAddress(LValAST* lval, const std::vector<int> &indices);
    Value* emitTensorElement(ExprAST* expr, const std::vector<int> &indices);
    bool lowerTensorAssignment(AssignStmtAST &node);
    Constant* evaluateConstantExpr(Value* val);

    // Symbol stack: Variable name -> Value* in IR.
    // (usually AllocaInst* address)
    std::vector<std::map<std::string, Value*>> Scopes;
    std::vector<std::map<std::string, TensorInfo>> TensorScopes;

    int ValueCounter = 0;
    int LabelCounter = 0;

    void enterScope() { 
        Scopes.emplace_back();
        TensorScopes.emplace_back();
    }
    void exitScope() { 
        Scopes.pop_back(); 
        TensorScopes.pop_back();
    }
    void defineVar(const std::string &name, Value *val);
    Value* lookupVar(const std::string &name);

    std::string nextValueName() { return "%" + std::to_string(ValueCounter++); }
    std::string newLabelName() { return "bb" + std::to_string(LabelCounter++); }
};

} 
#endif