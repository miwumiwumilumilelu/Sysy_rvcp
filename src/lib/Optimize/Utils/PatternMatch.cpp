#include "Optimize/Utils/PatternMatch.h"
#include "IR/Module.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>
#include <vector>

namespace sysy {

// x + 0 -> x
// Parse ( rewrite ( add x 0 ) x )
// -> S-expression AST :
// 
// list
//   atom rewrite
//   list
//     atom add
//     atom x
//     atom 0
//   atom x

struct Pattern::Expr {
    bool atom = true;
    std::string value;
    std::vector<std::unique_ptr<Expr>> elements;
};

Pattern::Pattern(const std::string& text) : Text(text) {
    Root = parse();
}

Pattern::~Pattern() = default;

static bool parseIntLiteral(const std::string& text, int& value) {
    if (text.empty())
        return false;
    char* end = nullptr;
    long v = std::strtol(text.c_str(), &end, 10);
    if (*end != '\0')
        return false;
    value = static_cast<int>(v);
    return true;
}

static bool parseFloatLiteral(const std::string& text, float& value) {
    if (text.size() < 2 || text[0] != '*')
        return false;
    char* end = nullptr;
    float v = std::strtof(text.c_str() + 1, &end);
    if (*end != '\0')
        return false;
    value = v;
    return true;
}

static bool sameConstInt(Value* lhs, Value* rhs) {
    auto* lc = dyn_cast<ConstantInt>(lhs);
    auto* rc = dyn_cast<ConstantInt>(rhs);
    return lc && rc && lc->getValue() == rc->getValue();
}

static bool sameConstFloat(Value* lhs, Value* rhs) {
    auto* lc = dyn_cast<ConstantFloat>(lhs);
    auto* rc = dyn_cast<ConstantFloat>(rhs);
    return lc && rc && lc->getValue() == rc->getValue();
}

std::string Pattern::nextToken() {
    while (Loc < Text.size() && std::isspace(static_cast<unsigned char>(Text[Loc])))
        ++Loc;
    if (Loc >= Text.size())
        return "";
    if (Text[Loc] == '(' || Text[Loc] == ')')
        return std::string(1, Text[Loc++]);

    size_t start = Loc;
    while (Loc < Text.size() &&
           !std::isspace(static_cast<unsigned char>(Text[Loc])) &&
           Text[Loc] != '(' && Text[Loc] != ')')
        ++Loc;
    return Text.substr(start, Loc - start);
}

std::unique_ptr<Pattern::Expr> Pattern::parse() {
    auto tok = nextToken();
    auto expr = std::make_unique<Expr>();
    if (tok == "(") {
        expr->atom = false;
        while (true) {
            size_t save = Loc;
            auto peek = nextToken();
            if (peek.empty() || peek == ")")
                break;
            Loc = save;
            expr->elements.push_back(parse());
        }
        return expr;
    }

    expr->atom = true;
    expr->value = tok;
    return expr;
}

bool Pattern::match(Value* value, const Binding& external) {
    Bindings = external;
    return Root && matchExpr(Root.get(), value);
}

Value* Pattern::extract(const std::string& name) const {
    auto it = Bindings.find(name);
    return it == Bindings.end() ? nullptr : it->second;
}

bool Pattern::extractInt(const std::string& name, int& value) const {
    auto* v = extract(name);
    auto* c = dyn_cast<ConstantInt>(v);
    if (!c)
        return false;
    value = c->getValue();
    return true;
}

bool Pattern::matchExpr(const Expr* expr, Value* value) {
    if (!expr)
        return false;
    if (!expr->atom)
        return matchList(expr, value);

    const std::string& atom = expr->value;
    if (atom.empty())
        return false;

    int literal = 0;
    if (parseIntLiteral(atom, literal)) {
        auto* c = dyn_cast<ConstantInt>(value);
        return c && c->getValue() == literal;
    }

    if (atom[0] == '\'') {
        auto* c = dyn_cast<ConstantInt>(value);
        if (!c)
            return false;
        auto it = Bindings.find(atom);
        if (it != Bindings.end())
            return sameConstInt(it->second, value);
        Bindings[atom] = value;
        return true;
    }

    if (atom[0] == '*') {
        auto* c = dyn_cast<ConstantFloat>(value);
        if (!c)
            return false;
        float literal = 0.0f;
        if (parseFloatLiteral(atom, literal) && c->getValue() != literal)
            return false;
        auto it = Bindings.find(atom);
        if (it != Bindings.end())
            return sameConstFloat(it->second, value);
        Bindings[atom] = value;
        return true;
    }

    auto it = Bindings.find(atom);
    if (it != Bindings.end())
        return it->second == value;
    Bindings[atom] = value;
    return true;
}

static bool isBinaryOp(Instruction* inst, Instruction::OpID op) {
    return inst && inst->getOpID() == op && inst->getNumOperands() == 2;
}

bool Pattern::matchList(const Expr* expr, Value* value) {
    if (expr->elements.empty() || !expr->elements[0]->atom)
        return false;

    const std::string& op = expr->elements[0]->value;
    auto* inst = dyn_cast<Instruction>(value);

    auto matchOrderedBin = [&](Instruction::OpID id) {
        if (!isBinaryOp(inst, id) || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), inst->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), inst->getOperand(1)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    auto matchCommutativeBin = [&](Instruction::OpID id) {
        if (!isBinaryOp(inst, id) || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), inst->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), inst->getOperand(1)))
            return true;
        Bindings = saved;
        if (matchExpr(expr->elements[1].get(), inst->getOperand(1)) &&
            matchExpr(expr->elements[2].get(), inst->getOperand(0)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    if (op == "add") return matchCommutativeBin(Instruction::Add);
    if (op == "sub") return matchOrderedBin(Instruction::Sub);
    if (op == "mul") return matchCommutativeBin(Instruction::Mul);
    if (op == "div") return matchOrderedBin(Instruction::Div);
    if (op == "mod") return matchOrderedBin(Instruction::Mod);
    if (op == "shl") return matchOrderedBin(Instruction::Shl);
    if (op == "ashr") return matchOrderedBin(Instruction::Ashr);
    if (op == "and") return matchCommutativeBin(Instruction::And);
    if (op == "or") return matchCommutativeBin(Instruction::Or);
    if (op == "xor") return matchCommutativeBin(Instruction::Xor);
    if (op == "fadd") return matchCommutativeBin(Instruction::FAdd);
    if (op == "fsub") return matchOrderedBin(Instruction::FSub);
    if (op == "fmul") return matchCommutativeBin(Instruction::FMul);
    if (op == "fdiv") return matchOrderedBin(Instruction::FDiv);

    if (op == "load") {
        auto* ld = dyn_cast<LoadInst>(value);
        return ld && expr->elements.size() == 2 &&
               matchExpr(expr->elements[1].get(), ld->getOperand(0));
    }

    if (op == "store") {
        auto* st = dyn_cast<StoreInst>(value);
        return st && expr->elements.size() == 3 &&
               matchExpr(expr->elements[1].get(), st->getOperand(0)) &&
               matchExpr(expr->elements[2].get(), st->getOperand(1));
    }

    if (op == "gep") {
        if (!inst || inst->getOpID() != Instruction::GetElementPtr ||
            expr->elements.size() != static_cast<size_t>(inst->getNumOperands() + 1))
            return false;
        auto saved = Bindings;
        for (int i = 0; i < inst->getNumOperands(); ++i) {
            if (!matchExpr(expr->elements[i + 1].get(), inst->getOperand(i))) {
                Bindings = std::move(saved);
                return false;
            }
        }
        return true;
    }

    auto matchOrderedCmp = [&](ICmpInst::CmpOp pred) {
        auto* cmp = dyn_cast<ICmpInst>(value);
        if (!cmp || cmp->getPredicate() != pred || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), cmp->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), cmp->getOperand(1)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    auto matchCommutativeCmp = [&](ICmpInst::CmpOp pred) {
        auto* cmp = dyn_cast<ICmpInst>(value);
        if (!cmp || cmp->getPredicate() != pred || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), cmp->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), cmp->getOperand(1)))
            return true;
        Bindings = saved;
        if (matchExpr(expr->elements[1].get(), cmp->getOperand(1)) &&
            matchExpr(expr->elements[2].get(), cmp->getOperand(0)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    if (op == "eq") return matchCommutativeCmp(ICmpInst::EQ);
    if (op == "ne") return matchCommutativeCmp(ICmpInst::NE);
    if (op == "sgt" || op == "gt") return matchOrderedCmp(ICmpInst::SGT);
    if (op == "sge" || op == "ge") return matchOrderedCmp(ICmpInst::SGE);
    if (op == "slt" || op == "lt") return matchOrderedCmp(ICmpInst::SLT);
    if (op == "sle" || op == "le") return matchOrderedCmp(ICmpInst::SLE);

    if (op == "sitofp") {
        return inst && inst->getOpID() == Instruction::SIToFP &&
               expr->elements.size() == 2 &&
               matchExpr(expr->elements[1].get(), inst->getOperand(0));
    }
    if (op == "fptosi") {
        return inst && inst->getOpID() == Instruction::FPToSI &&
               expr->elements.size() == 2 &&
               matchExpr(expr->elements[1].get(), inst->getOperand(0));
    }

    return false;
}

struct Match::Expr {
    bool atom = true;
    std::string value;
    std::vector<std::unique_ptr<Expr>> elements;
};

Match::Match(const std::string& text) : Text(text) {
    parseRewrite();
}

Match::~Match() = default;
Match::Match(Match&& other) noexcept = default;
Match& Match::operator=(Match&& other) noexcept = default;

static bool evalIntOp(const std::string& op, int lhs, int rhs, int& out);

std::string Match::nextToken() {
    while (Loc < Text.size() && std::isspace(static_cast<unsigned char>(Text[Loc])))
        ++Loc;
    if (Loc >= Text.size())
        return "";
    if (Text[Loc] == '(' || Text[Loc] == ')')
        return std::string(1, Text[Loc++]);

    size_t start = Loc;
    while (Loc < Text.size() &&
           !std::isspace(static_cast<unsigned char>(Text[Loc])) &&
           Text[Loc] != '(' && Text[Loc] != ')')
        ++Loc;
    return Text.substr(start, Loc - start);
}

std::unique_ptr<Match::Expr> Match::parse() {
    auto tok = nextToken();
    auto expr = std::make_unique<Expr>();
    if (tok == "(") {
        expr->atom = false;
        while (true) {
            size_t save = Loc;
            auto peek = nextToken();
            if (peek.empty() || peek == ")")
                break;
            Loc = save;
            expr->elements.push_back(parse());
        }
        return expr;
    }

    expr->atom = true;
    expr->value = tok;
    return expr;
}

bool Match::parseRewrite() {
    auto root = parse();
    if (!root || root->atom)
        return false;
    if (root->elements.size() == 3 &&
        root->elements[0]->atom && root->elements[0]->value == "rewrite") {
        From = std::move(root->elements[1]);
        To = std::move(root->elements[2]);
        return true;
    }
    if (root->elements.size() == 4 &&
        root->elements[0]->atom && root->elements[0]->value == "rewrite-if") {
        Condition = std::move(root->elements[1]);
        From = std::move(root->elements[2]);
        To = std::move(root->elements[3]);
        return true;
    }
    return false;
}

bool Match::evalConstInt(const Expr* expr, int& value) {
    if (!expr)
        return false;
    if (expr->atom) {
        if (parseIntLiteral(expr->value, value))
            return true;
        auto it = Bindings.find(expr->value);
        if (it != Bindings.end()) {
            if (auto* ci = dyn_cast<ConstantInt>(it->second)) {
                value = ci->getValue();
                return true;
            }
        }
        return false;
    }

    if (expr->elements.empty() || !expr->elements[0]->atom)
        return false;

    const std::string& op = expr->elements[0]->value;
    if (op.empty() || op[0] != '!')
        return false;

    if (expr->elements.size() == 2) {
        int a = 0;
        if (!evalConstInt(expr->elements[1].get(), a))
            return false;
        if (op == "!not") { value = !a; return true; }
        if (op == "!pow2") { value = a > 0 && (a & (a - 1)) == 0; return true; }
        return false;
    }

    if (expr->elements.size() != 3)
        return false;

    int lhs = 0, rhs = 0;
    if (!evalConstInt(expr->elements[1].get(), lhs) ||
        !evalConstInt(expr->elements[2].get(), rhs))
        return false;
    return evalIntOp(op, lhs, rhs, value);
}

bool Match::evalConstFloat(const Expr* expr, float& value) {
    if (!expr)
        return false;
    if (expr->atom) {
        if (parseFloatLiteral(expr->value, value))
            return true;
        auto it = Bindings.find(expr->value);
        if (it != Bindings.end()) {
            if (auto* cf = dyn_cast<ConstantFloat>(it->second)) {
                value = cf->getValue();
                return true;
            }
        }
        return false;
    }

    if (expr->elements.size() != 3 || !expr->elements[0]->atom)
        return false;
    const std::string& op = expr->elements[0]->value;
    if (op.empty() || op[0] != '?')
        return false;
    float lhs = 0.0f, rhs = 0.0f;
    if (!evalConstFloat(expr->elements[1].get(), lhs) ||
        !evalConstFloat(expr->elements[2].get(), rhs))
        return false;
    if (op == "?add") { value = lhs + rhs; return true; }
    if (op == "?sub") { value = lhs - rhs; return true; }
    if (op == "?mul") { value = lhs * rhs; return true; }
    if (op == "?div") { if (rhs == 0.0f) return false; value = lhs / rhs; return true; }
    return false;
}

bool Match::matchExpr(const Expr* expr, Value* value) {
    if (!expr)
        return false;
    if (!expr->atom)
        return matchList(expr, value);

    const std::string& atom = expr->value;
    if (atom.empty())
        return false;

    int literal = 0;
    if (parseIntLiteral(atom, literal)) {
        auto* c = dyn_cast<ConstantInt>(value);
        return c && c->getValue() == literal;
    }

    if (atom[0] == '\'') {
        auto* c = dyn_cast<ConstantInt>(value);
        if (!c)
            return false;
        auto it = Bindings.find(atom);
        if (it != Bindings.end())
            return sameConstInt(it->second, value);
        Bindings[atom] = value;
        return true;
    }

    if (atom[0] == '*') {
        auto* c = dyn_cast<ConstantFloat>(value);
        if (!c)
            return false;
        float literal = 0.0f;
        if (parseFloatLiteral(atom, literal) && c->getValue() != literal)
            return false;
        auto it = Bindings.find(atom);
        if (it != Bindings.end())
            return sameConstFloat(it->second, value);
        Bindings[atom] = value;
        return true;
    }

    auto it = Bindings.find(atom);
    if (it != Bindings.end())
        return it->second == value;
    Bindings[atom] = value;
    return true;
}

bool Match::matchList(const Expr* expr, Value* value) {
    if (expr->elements.empty() || !expr->elements[0]->atom)
        return false;

    const std::string& op = expr->elements[0]->value;
    auto* inst = dyn_cast<Instruction>(value);

    auto matchOrderedBin = [&](Instruction::OpID id) {
        if (!isBinaryOp(inst, id) || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), inst->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), inst->getOperand(1)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    auto matchCommutativeBin = [&](Instruction::OpID id) {
        if (!isBinaryOp(inst, id) || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), inst->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), inst->getOperand(1)))
            return true;
        Bindings = saved;
        if (matchExpr(expr->elements[1].get(), inst->getOperand(1)) &&
            matchExpr(expr->elements[2].get(), inst->getOperand(0)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    if (op == "add") return matchCommutativeBin(Instruction::Add);
    if (op == "sub") return matchOrderedBin(Instruction::Sub);
    if (op == "mul") return matchCommutativeBin(Instruction::Mul);
    if (op == "div") return matchOrderedBin(Instruction::Div);
    if (op == "mod") return matchOrderedBin(Instruction::Mod);
    if (op == "shl") return matchOrderedBin(Instruction::Shl);
    if (op == "ashr") return matchOrderedBin(Instruction::Ashr);
    if (op == "and") return matchCommutativeBin(Instruction::And);
    if (op == "or") return matchCommutativeBin(Instruction::Or);
    if (op == "xor") return matchCommutativeBin(Instruction::Xor);

    auto matchOrderedCmp = [&](ICmpInst::CmpOp pred) {
        auto* cmp = dyn_cast<ICmpInst>(value);
        if (!cmp || cmp->getPredicate() != pred || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), cmp->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), cmp->getOperand(1)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    auto matchCommutativeCmp = [&](ICmpInst::CmpOp pred) {
        auto* cmp = dyn_cast<ICmpInst>(value);
        if (!cmp || cmp->getPredicate() != pred || expr->elements.size() != 3)
            return false;
        auto saved = Bindings;
        if (matchExpr(expr->elements[1].get(), cmp->getOperand(0)) &&
            matchExpr(expr->elements[2].get(), cmp->getOperand(1)))
            return true;
        Bindings = saved;
        if (matchExpr(expr->elements[1].get(), cmp->getOperand(1)) &&
            matchExpr(expr->elements[2].get(), cmp->getOperand(0)))
            return true;
        Bindings = std::move(saved);
        return false;
    };

    if (op == "eq") return matchCommutativeCmp(ICmpInst::EQ);
    if (op == "ne") return matchCommutativeCmp(ICmpInst::NE);
    if (op == "sgt" || op == "gt") return matchOrderedCmp(ICmpInst::SGT);
    if (op == "sge" || op == "ge") return matchOrderedCmp(ICmpInst::SGE);
    if (op == "slt" || op == "lt") return matchOrderedCmp(ICmpInst::SLT);
    if (op == "sle" || op == "le") return matchOrderedCmp(ICmpInst::SLE);

    return false;
}

static bool opFromName(const std::string& name, Instruction::OpID& op) {
    if (name == "add") { op = Instruction::Add; return true; }
    if (name == "sub") { op = Instruction::Sub; return true; }
    if (name == "mul") { op = Instruction::Mul; return true; }
    if (name == "div") { op = Instruction::Div; return true; }
    if (name == "mod") { op = Instruction::Mod; return true; }
    if (name == "shl") { op = Instruction::Shl; return true; }
    if (name == "ashr") { op = Instruction::Ashr; return true; }
    if (name == "and") { op = Instruction::And; return true; }
    if (name == "or")  { op = Instruction::Or;  return true; }
    if (name == "xor") { op = Instruction::Xor; return true; }
    if (name == "fadd") { op = Instruction::FAdd; return true; }
    if (name == "fsub") { op = Instruction::FSub; return true; }
    if (name == "fmul") { op = Instruction::FMul; return true; }
    if (name == "fdiv") { op = Instruction::FDiv; return true; }
    return false;
}

static bool cmpFromName(const std::string& name, ICmpInst::CmpOp& pred) {
    if (name == "eq") { pred = ICmpInst::EQ; return true; }
    if (name == "ne") { pred = ICmpInst::NE; return true; }
    if (name == "slt" || name == "lt") { pred = ICmpInst::SLT; return true; }
    if (name == "sle" || name == "le") { pred = ICmpInst::SLE; return true; }
    if (name == "sgt" || name == "gt") { pred = ICmpInst::SGT; return true; }
    if (name == "sge" || name == "ge") { pred = ICmpInst::SGE; return true; }
    return false;
}

static bool evalIntOp(const std::string& op, int lhs, int rhs, int& out) {
    if (op == "!add") { out = lhs + rhs; return true; }
    if (op == "!sub") { out = lhs - rhs; return true; }
    if (op == "!mul") { out = lhs * rhs; return true; }
    if (op == "!div") { if (rhs == 0) return false; out = lhs / rhs; return true; }
    if (op == "!mod") { if (rhs == 0) return false; out = lhs % rhs; return true; }
    if (op == "!shl") { if (rhs < 0 || rhs >= 32) return false; out = static_cast<int>(static_cast<unsigned>(lhs) << rhs); return true; }
    if (op == "!ashr") { if (rhs < 0 || rhs >= 32) return false; out = lhs >> rhs; return true; }
    if (op == "!and") { out = lhs & rhs; return true; }
    if (op == "!or")  { out = lhs | rhs; return true; }
    if (op == "!xor") { out = lhs ^ rhs; return true; }
    if (op == "!min") { out = std::min(lhs, rhs); return true; }
    if (op == "!max") { out = std::max(lhs, rhs); return true; }
    if (op == "!eq")  { out = lhs == rhs; return true; }
    if (op == "!ne")  { out = lhs != rhs; return true; }
    if (op == "!lt")  { out = lhs < rhs; return true; }
    if (op == "!le")  { out = lhs <= rhs; return true; }
    if (op == "!gt")  { out = lhs > rhs; return true; }
    if (op == "!ge")  { out = lhs >= rhs; return true; }
    return false;
}

Value* Match::evalConstExpr(const Expr* expr) {
    if (!expr)
        return nullptr;
    if (expr->atom) {
        int literal = 0;
        if (parseIntLiteral(expr->value, literal))
            return new ConstantInt(literal);
        float fliteral = 0.0f;
        if (parseFloatLiteral(expr->value, fliteral))
            return new ConstantFloat(fliteral);
        auto it = Bindings.find(expr->value);
        if (it != Bindings.end() &&
            (dyn_cast<ConstantInt>(it->second) || dyn_cast<ConstantFloat>(it->second)))
            return it->second;
        return nullptr;
    }

    if (expr->elements.size() != 3 || !expr->elements[0]->atom)
        return nullptr;

    const std::string& op = expr->elements[0]->value;
    if (op.empty() || (op[0] != '!' && op[0] != '?'))
        return nullptr;

    if (op[0] == '?') {
        float value = 0.0f;
        if (!evalConstFloat(expr, value))
            return nullptr;
        return new ConstantFloat(value);
    }

    int value = 0;
    if (!evalConstInt(expr, value))
        return nullptr;
    return new ConstantInt(value);
}

Value* Match::buildExpr(const Expr* expr, Instruction* before) {
    if (!expr)
        return nullptr;
    if (expr->atom) {
        int literal = 0;
        if (parseIntLiteral(expr->value, literal))
            return new ConstantInt(literal);
        float fliteral = 0.0f;
        if (parseFloatLiteral(expr->value, fliteral))
            return new ConstantFloat(fliteral);
        auto it = Bindings.find(expr->value);
        return it == Bindings.end() ? nullptr : it->second;
    }

    if (expr->elements.empty() || !expr->elements[0]->atom)
        return nullptr;

    const std::string& opName = expr->elements[0]->value;
    if (!opName.empty() && (opName[0] == '!' || opName[0] == '?'))
        return evalConstExpr(expr);
    if (expr->elements.size() != 3)
        return nullptr;

    Value* lhs = buildExpr(expr->elements[1].get(), before);
    Value* rhs = buildExpr(expr->elements[2].get(), before);
    if (!lhs || !rhs)
        return nullptr;

    auto& insts = before->getParent()->getInstructions();
    auto pos = std::find(insts.begin(), insts.end(), before);

    Instruction::OpID op;
    if (opFromName(opName, op)) {
        auto* bin = new BinaryInst(op, lhs, rhs, nullptr);
        bin->setParent(before->getParent());
        insts.insert(pos, bin);
        return bin;
    }

    ICmpInst::CmpOp pred;
    if (cmpFromName(opName, pred)) {
        auto* cmp = new ICmpInst(pred, lhs, rhs, nullptr);
        cmp->setParent(before->getParent());
        insts.insert(pos, cmp);
        return cmp;
    }

    return nullptr;
}

bool Match::rewrite(Instruction* inst) {
    if (!From || !To || !inst || !inst->getParent())
        return false;
    Bindings.clear();
    if (!matchExpr(From.get(), inst))
        return false;
    if (Condition) {
        int ok = 0;
        if (!evalConstInt(Condition.get(), ok) || !ok)
            return false;
    }

    Value* replacement = buildExpr(To.get(), inst);
    if (!replacement || replacement == inst)
        return false;

    inst->replaceAllUsesWith(replacement);
    inst->eraseInst();
    return true;
}

}
