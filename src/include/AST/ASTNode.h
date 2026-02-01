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
    void dump(int indent) const override;
    void accept(ASTVisitor &visitor) override;
};

class LValAST : public ExprAST {
    std::string Name;
public:
    LValAST(const std::string &name) : Name(name) {}
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

class VarDeclAST : public ASTNode {
    std::string Type;
    std::string Name;
    std::unique_ptr<ExprAST> InitExpr;
public:
    VarDeclAST(const std::string &type, const std::string &name, std::unique_ptr<ExprAST> init)
        : Type(type), Name(name), InitExpr(std::move(init)) {}

    const std::string& getType() const { return Type; }
    const std::string& getName() const { return Name; }
    ExprAST* getInit() const { return InitExpr.get(); }

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

class FuncDefAST : public ASTNode {
    std::string Name;
    std::string RetType; // int, float, void
    std::unique_ptr<BlockAST> Body;
    // std::vector<std::unique_ptr<FuncFParamAST>> Params; 
public:
    FuncDefAST(const std::string &name, const std::string &retType, std::unique_ptr<BlockAST> body)
        : Name(name), RetType(retType), Body(std::move(body)) {}

    const std::string& getName() const { return Name; }
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