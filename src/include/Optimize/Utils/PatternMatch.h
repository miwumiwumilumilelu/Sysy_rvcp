#ifndef PATTERNMATCH_H
#define PATTERNMATCH_H

#include "IR/Instruction.h"
#include <map>
#include <memory>
#include <string>

namespace sysy {

class Pattern {
public:
    using Binding = std::map<std::string, Value*>;

    explicit Pattern(const std::string& text);
    ~Pattern();

    bool match(Value* value, const Binding& external = {});
    Value* extract(const std::string& name) const;
    bool extractInt(const std::string& name, int& value) const;

private:
    struct Expr;

    std::string Text;
    size_t Loc = 0;
    std::unique_ptr<Expr> Root;
    Binding Bindings;

    std::string nextToken();
    std::unique_ptr<Expr> parse();
    bool matchExpr(const Expr* expr, Value* value);
    bool matchList(const Expr* expr, Value* value);
};

class Match {
public:
    explicit Match(const std::string& text);
    ~Match();
    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;
    Match(Match&& other) noexcept;
    Match& operator=(Match&& other) noexcept;

    bool rewrite(Instruction* inst);

private:
    struct Expr;

    std::string Text;
    size_t Loc = 0;
    std::unique_ptr<Expr> From;
    std::unique_ptr<Expr> To;
    std::unique_ptr<Expr> Condition;
    Pattern::Binding Bindings;

    std::string nextToken();
    std::unique_ptr<Expr> parse();
    bool parseRewrite();

    bool matchExpr(const Expr* expr, Value* value);
    bool matchList(const Expr* expr, Value* value);
    Value* buildExpr(const Expr* expr, Instruction* before);
    Value* evalConstExpr(const Expr* expr);
    bool evalConstInt(const Expr* expr, int& value);
    bool evalConstFloat(const Expr* expr, float& value);
};

}

#endif
