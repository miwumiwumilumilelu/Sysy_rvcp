#ifndef PATTERNMATCH_H
#define PATTERNMATCH_H

#include "../../IR/Instruction.h"
#include <map>
#include <memory>
#include <string>

namespace sysy {

struct PMExpr;

class Pattern {
public:
    using Binding = std::map<std::string, Value*>;

    explicit Pattern(const std::string& text);
    ~Pattern();

    bool match(Value* value, const Binding& external = {});
    Value* extract(const std::string& name) const;
    bool extractInt(const std::string& name, int& value) const;

private:
    std::string Text;
    std::unique_ptr<PMExpr> Root;
    Binding Bindings;

    bool matchExpr(const PMExpr* expr, Value* value);
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
    std::string Text;
    std::unique_ptr<PMExpr> From;
    std::unique_ptr<PMExpr> To;
    std::unique_ptr<PMExpr> Condition;
    Pattern::Binding Bindings;

    bool parseRewrite();

    bool matchExpr(const PMExpr* expr, Value* value);
    Value* buildExpr(const PMExpr* expr, Instruction* before);
    Value* evalConstExpr(const PMExpr* expr);
    bool evalConstInt(const PMExpr* expr, int& value);
    bool evalConstFloat(const PMExpr* expr, float& value);
};

}

#endif
