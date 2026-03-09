#ifndef ASTNODE_H
#define ASTNODE_H

#include <memory>
#include <string>
#include <vector>
#include <iostream>

namespace sysy {

class ASTVisitor;

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void dump(int indent = 0) const = 0;
    virtual void accept(ASTVisitor &visitor) = 0;
};

class ExprAST : public ASTNode {};

class NumberAST : public ExprAST {
    enum { IntKind, FloatKind } Kind;
    union {
        int IntVal;
        float FloatVal;
    };
public:
    NumberAST(int val) : Kind(IntKind), IntVal(val) {}
    NumberAST(float val) : Kind(FloatKind), FloatVal(val) {}

    int getIntVal() const { return IntVal; }
    float getFloatVal() const { return FloatVal; }
    bool isInt() const { return Kind == IntKind; }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class FuncCallAST : public ExprAST {
    std::string Name;
    std::vector<std::unique_ptr<ExprAST>> Args;
public:
    FuncCallAST(const std::string &name, std::vector<std::unique_ptr<ExprAST>> args)
        : Name(name), Args(std::move(args)) {}

    const std::string& getName() const { return Name; }
    const std::vector<std::unique_ptr<ExprAST>>& getArgs() const { return Args; }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class LValAST : public ExprAST {
    std::string Name;
    // Support for array access.
    std::vector<std::unique_ptr<ExprAST>> Indices;
public:
    LValAST(const std::string &name, std::vector<std::unique_ptr<ExprAST>> indices = {})
        : Name(name), Indices(std::move(indices)) {}
    std::vector<std::unique_ptr<ExprAST>>& getIndices() { return Indices; }
    std::string getName() const { return Name; }
    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class BinaryExprAST : public ExprAST {
    std::string Op; // Use string to support "==", ">=" etc.
    std::unique_ptr<ExprAST> LHS, RHS;
public:
    BinaryExprAST(std::string op, std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs)
        : Op(op), LHS(std::move(lhs)), RHS(std::move(rhs)) {}
    
    const std::string& getOp() const { return Op; }
    ExprAST* getLHS() const { return LHS.get(); }
    ExprAST* getRHS() const { return RHS.get(); }
    
    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class UnaryExprAST : public ExprAST {
    std::string Op;
    std::unique_ptr<ExprAST> Operand;
public:
    UnaryExprAST(std::string op, std::unique_ptr<ExprAST> operand)
        : Op(op), Operand(std::move(operand)) {}

    const std::string& getOp() const { return Op; }
    ExprAST* getOperand() const { return Operand.get(); }
    
    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class InitValAST : public ASTNode {
    std::unique_ptr<ExprAST> Expr;
    std::vector<std::unique_ptr<InitValAST>> Elements;
    bool IsLeaf;
public:
    InitValAST(std::unique_ptr<ExprAST> expr) : Expr(std::move(expr)), IsLeaf(true) {}
    InitValAST(std::vector<std::unique_ptr<InitValAST>> elements) : Elements(std::move(elements)), IsLeaf(false) {}

    bool isLeaf() const { return IsLeaf; }
    ExprAST* getExpr() const { return Expr.get(); }
    const std::vector<std::unique_ptr<InitValAST>>& getElements() const { return Elements; }

    void dump(int indent) const override;
    void accept(ASTVisitor&) override {}
};

class VarDeclAST : public ASTNode {
    std::string Type;
    std::string Name;
    std::vector<std::unique_ptr<ExprAST>> Dims;
    std::unique_ptr<InitValAST> Init;
public:
    VarDeclAST(const std::string &type, const std::string &name, std::vector<std::unique_ptr<ExprAST>> dims, std::unique_ptr<InitValAST> init)
        : Type(type), Name(name), Dims(std::move(dims)), Init(std::move(init)) {}

    const std::string& getType() const { return Type; }
    const std::string& getName() const { return Name; }
    const std::vector<std::unique_ptr<ExprAST>>& getDims() const { return Dims; }
    InitValAST* getInit() const { return Init.get(); }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class StmtAST : public ASTNode {};

class ReturnStmtAST : public StmtAST {
    std::unique_ptr<ExprAST> RetVal;
public:
    ReturnStmtAST(std::unique_ptr<ExprAST> val) : RetVal(std::move(val)) {}

    ExprAST* getRetVal() const { return RetVal.get(); }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class BreakStmtAST : public StmtAST {
public:
    void accept(ASTVisitor &visitor) override;
    void dump(int indent) const override;
};

class ContinueStmtAST : public StmtAST {
public:
    void accept(ASTVisitor &visitor) override;
    void dump(int indent) const override;
};

class AssignStmtAST : public StmtAST {
    std::unique_ptr<LValAST> LVal;
    std::unique_ptr<ExprAST> Value;
public:
    AssignStmtAST(std::unique_ptr<LValAST> lval, std::unique_ptr<ExprAST> val)
        : LVal(std::move(lval)), Value(std::move(val)) {}

    LValAST* getLVal() const { return LVal.get(); }
    ExprAST* getValue() const { return Value.get(); }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class IfStmtAST : public StmtAST {
    std::unique_ptr<ExprAST> Cond;
    std::unique_ptr<StmtAST> Then, Else;
public:
    IfStmtAST(std::unique_ptr<ExprAST> cond, std::unique_ptr<StmtAST> thenStmt, std::unique_ptr<StmtAST> elseStmt)
        : Cond(std::move(cond)), Then(std::move(thenStmt)), Else(std::move(elseStmt)) {}
    
    ExprAST* getCond() const { return Cond.get(); }
    StmtAST* getThen() const { return Then.get(); }
    StmtAST* getElse() const { return Else.get(); }
    
    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class WhileStmtAST : public StmtAST {
    std::unique_ptr<ExprAST> Cond;
    std::unique_ptr<StmtAST> Body;
public:
    WhileStmtAST(std::unique_ptr<ExprAST> cond, std::unique_ptr<StmtAST> body)
        : Cond(std::move(cond)), Body(std::move(body)) {}

    ExprAST* getCond() const { return Cond.get(); }
    StmtAST* getBody() const { return Body.get(); }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class ExprStmtAST : public StmtAST {
    std::unique_ptr<ExprAST> Expr;
public:
    ExprStmtAST(std::unique_ptr<ExprAST> expr) : Expr(std::move(expr)) {}

    ExprAST* getExpr() const { return Expr.get(); }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class BlockAST : public StmtAST {
    std::vector<std::unique_ptr<ASTNode>> Items; // 包含 Stmt 或 Decl
public:
    void addItem(std::unique_ptr<ASTNode> item) { Items.push_back(std::move(item)); }
    
    const std::vector<std::unique_ptr<ASTNode>>& getItems() const { return Items; }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class FuncFParamAST : public ASTNode {
    std::string Type;
    std::string Name;
    std::vector<std::unique_ptr<ExprAST>> Dims;
public:
    FuncFParamAST(const std::string &type, const std::string &name, std::vector<std::unique_ptr<ExprAST>> dims)
        : Type(type), Name(name), Dims(std::move(dims)) {}

    const std::string& getType() const { return Type; }
    const std::string& getName() const { return Name; }
    const std::vector<std::unique_ptr<ExprAST>>& getDims() const { return Dims; }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class FuncDefAST : public ASTNode {
    std::string Name;
    std::string RetType; // int, float, void
    std::vector<std::unique_ptr<FuncFParamAST>> Params; 
    std::unique_ptr<BlockAST> Body;
public:
    FuncDefAST(const std::string &name, const std::string &retType, 
        std::vector<std::unique_ptr<FuncFParamAST>> params, std::unique_ptr<BlockAST> body)
        : Name(name), RetType(retType), Params(std::move(params)), Body(std::move(body)) {}

    const std::string& getName() const { return Name; }
    const std::string& getRetType() const { return RetType; }
    const std::vector<std::unique_ptr<FuncFParamAST>>& getParams() const { return Params; }
    BlockAST* getBody() const { return Body.get(); }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class CompUnitAST : public ASTNode {
    std::vector<std::unique_ptr<ASTNode>> Children;
public:
    void addChild(std::unique_ptr<ASTNode> child) {
        Children.push_back(std::move(child));
    }

    const std::vector<std::unique_ptr<ASTNode>>& getChildren() const { return Children; }

    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

}

#endif