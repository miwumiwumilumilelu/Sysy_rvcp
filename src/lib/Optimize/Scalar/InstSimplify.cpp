#include "Optimize/Scalar/InstSimplify.h"
#include "Optimize/Analysis/Range.h"
#include "Optimize/Utils/PatternMatch.h"
#include <algorithm>
#include <memory>

using namespace sysy;

bool InstSimplify::run() {
    bool anyChanged = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto func : TheModule->getFunctions()) {
            for (auto bb : func->getBody()->getBlocks()) {
                if (simplify(bb)) { 
                    changed = true;
                    anyChanged = true; 
                }
            }
        }
    }
    return anyChanged;
}

// Match table for local expression rewrites.
static Match regularMatches[] = {
    Match("(rewrite (add 'a 'b) (!add 'a 'b))"),
    Match("(rewrite (add x 0) x)"),
    Match("(rewrite (add x (sub y x)) y)"),
    Match("(rewrite (add (sub x 'a) 'a) x)"),

    Match("(rewrite (sub 'a 'b) (!sub 'a 'b))"),
    Match("(rewrite (sub x 0) x)"),
    Match("(rewrite (sub x x) 0)"),
    Match("(rewrite (sub (add x 'a) 'a) x)"),
    Match("(rewrite (sub (add x y) x) y)"),
    Match("(rewrite (sub (add x y) y) x)"),

    Match("(rewrite (mul 'a 'b) (!mul 'a 'b))"),
    Match("(rewrite (mul x 1) x)"),
    Match("(rewrite (mul x 0) 0)"),

    Match("(rewrite (div 'a 'b) (!div 'a 'b))"),
    Match("(rewrite (div x 1) x)"),
    Match("(rewrite (div 0 x) 0)"),

    Match("(rewrite (mod 'a 'b) (!mod 'a 'b))"),
    Match("(rewrite (mod x 1) 0)"),

    Match("(rewrite (shl x 0) x)"),
    Match("(rewrite (shl 0 x) 0)"),
    Match("(rewrite (ashr x 0) x)"),
    Match("(rewrite (ashr 0 x) 0)"),

    Match("(rewrite (and x 0) 0)"),
    Match("(rewrite (and x -1) x)"),
    Match("(rewrite (and x x) x)"),

    Match("(rewrite (or x 0) x)"),
    Match("(rewrite (or x -1) -1)"),
    Match("(rewrite (or x x) x)"),

    Match("(rewrite (xor x 0) x)"),
    Match("(rewrite (xor x x) 0)"),

    Match("(rewrite (fadd *a *b) (?add *a *b))"),
    Match("(rewrite (fadd x *0) x)"),
    Match("(rewrite (fsub *a *b) (?sub *a *b))"),
    Match("(rewrite (fsub x *0) x)"),
    Match("(rewrite (fmul *a *b) (?mul *a *b))"),
    Match("(rewrite (fmul x *1) x)"),
    Match("(rewrite (fmul x *0) *0)"),
    Match("(rewrite (fdiv *a *b) (?div *a *b))"),
    Match("(rewrite (fdiv x *1) x)"),
    Match("(rewrite (fdiv *0 x) *0)"),

    Match("(rewrite-if (!and (!and (!ge 'a 0) (!ge 'b 0)) (!lt (!add 'a 'b) 32)) (shl (shl x 'a) 'b) (shl x (!add 'a 'b)))"),
    Match("(rewrite-if (!and (!and (!ge 'a 0) (!ge 'b 0)) (!ge (!add 'a 'b) 32)) (shl (shl x 'a) 'b) 0)"),
    Match("(rewrite-if (!and (!ge 'a 0) (!ge 'b 0)) (ashr (ashr x 'a) 'b) (ashr x (!min (!add 'a 'b) 31)))"),
    Match("(rewrite (and (and x 'a) 'b) (and x (!and 'a 'b)))"),
    Match("(rewrite (or (or x 'a) 'b) (or x (!or 'a 'b)))"),
    Match("(rewrite (xor (xor x 'a) 'b) (xor x (!xor 'a 'b)))"),

    Match("(rewrite-if (!and (!and (!gt 'shift 0) (!lt 'shift 32)) (!and (!ge 'mask 0) (!eq (!ashr 'mask 'shift) 0))) (ashr (and x 'mask) 'shift) 0)"),

    Match("(rewrite (or (and X 'c1) (and X 'c2)) (and X (!or 'c1 'c2)))"),
    Match("(rewrite (or (and X 'c) (and Y 'c)) (and (or X Y) 'c))"),
    Match("(rewrite (xor (and X 'c) (and Y 'c)) (and (xor X Y) 'c))"),

    Match("(rewrite (ne (mod X 2) 0) (ne (and X 1) 0))"),
    Match("(rewrite (eq (mod X 2) 0) (eq (and X 1) 0))"),
    Match("(rewrite (ne (and X 'm) (and Y 'm)) (ne (and (xor X Y) 'm) 0))"),
    Match("(rewrite-if (!pow2 'm) (and (ne (and X 'm) 0) (ne (and Y 'm) 0)) (ne (and (and X Y) 'm) 0))"),
    Match("(rewrite-if (!ge 'k 0) (and (ashr X 'k) (ashr Y 'k)) (ashr (and X Y) 'k))"),
    Match("(rewrite-if (!ge 'k 0) (or (ashr X 'k) (ashr Y 'k)) (ashr (or X Y) 'k))"),
    Match("(rewrite-if (!ge 'k 0) (xor (ashr X 'k) (ashr Y 'k)) (ashr (xor X Y) 'k))"),
    Match("(rewrite-if (!and (!ge 'k 0) (!lt 'k 32)) (shl (and (ashr X 'k) 1) 'k) (and X (!shl 1 'k)))"),
};

static bool applyRegularMatches(Instruction* inst) {
    for (auto& match : regularMatches) {
        if (match.rewrite(inst))
            return true;
    }
    return false;
}

bool InstSimplify::simplify(BasicBlock* bb) {
    bool changed = false;
    std::unique_ptr<RangeAnalysis> range;
    auto isNonNeg = [&](Value* v) -> bool {
        if (auto* ci = dyn_cast<ConstantInt>(v))
            return ci->getValue() >= 0;
        if (!range) {
            auto* f = bb->getParentFunc();
            if (!f) return false;
            range = std::make_unique<RangeAnalysis>(f);
        }
        return range->isNonNeg(v);
    };

    std::vector<Instruction*> worklist(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto inst : worklist) {
        if (!inst->getParent()) continue;
        if (applyRegularMatches(inst)) {
            changed = true;
            continue;
        }
    }

    auto insertBefore = [&](Instruction* pos_inst, Instruction* newInst) {
        auto& il = bb->getInstructions();
        auto it = std::find(il.begin(), il.end(), pos_inst);
        il.insert(it, newInst);
        newInst->setParent(bb);
    };

    std::vector<Instruction*> wl2(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto* inst : wl2) {
        if (!inst->getParent()) continue;

        // X % 2 != Y % 2 -> (X^Y) & 1 != 0
        // This is sound only when both X and Y are proven non-negative.
        auto* cmp = dyn_cast<ICmpInst>(inst);
        if (!cmp || cmp->getPredicate() != ICmpInst::NE)
            continue;

        auto isMod2 = [](Value* v) -> Value* {
            auto* b = dyn_cast<BinaryInst>(v);
            if (!b || b->getOpID() != Instruction::Mod) return nullptr;
            auto* c = dyn_cast<ConstantInt>(b->getOperand(1));
            return (c && c->getValue() == 2) ? b->getOperand(0) : nullptr;
        };

        Value* X = isMod2(cmp->getOperand(0));
        Value* Y = isMod2(cmp->getOperand(1));
        if (!X || !Y || !isNonNeg(X) || !isNonNeg(Y))
            continue;

        auto* xorInst = new BinaryInst(Instruction::Xor, X, Y, nullptr);
        xorInst->setName("^");
        insertBefore(inst, xorInst);
        auto* andInst = new BinaryInst(Instruction::And, xorInst, new ConstantInt(1), nullptr);
        andInst->setName("(^)&");
        insertBefore(inst, andInst);
        auto* newCmp = new ICmpInst(ICmpInst::NE, andInst, new ConstantInt(0), nullptr);
        newCmp->setName(cmp->getName());
        insertBefore(inst, newCmp);
        cmp->replaceAllUsesWith(newCmp);
        cmp->eraseInst();
        changed = true;
    }

    return changed;
}
