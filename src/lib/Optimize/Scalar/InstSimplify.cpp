#include "../../../include/Optimize/Scalar/InstSimplify.h"
#include "../../../include/Optimize/Analysis/ValueTracking.h"
#include "../../../include/Optimize/Utils/PatternMatch.h"
#include <algorithm>
#include <functional>
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

    Match("(rewrite (eq (and X 1) 1) (and X 1))"),
    Match("(rewrite (ne (and X 1) 0) (and X 1))"),
    Match("(rewrite (ne (mod X 2) 0) (ne (and X 1) 0))"),
    Match("(rewrite (eq (mod X 2) 0) (eq (and X 1) 0))"),
    Match("(rewrite (ne (and X 'm) (and Y 'm)) (ne (and (xor X Y) 'm) 0))"),
    Match("(rewrite-if (!pow2 'm) (and (ne (and X 'm) 0) (ne (and Y 'm) 0)) (ne (and (and X Y) 'm) 0))"),
    Match("(rewrite-if (!ge 'k 0) (and (ashr X 'k) (ashr Y 'k)) (ashr (and X Y) 'k))"),
    Match("(rewrite-if (!ge 'k 0) (or (ashr X 'k) (ashr Y 'k)) (ashr (or X Y) 'k))"),
    Match("(rewrite-if (!ge 'k 0) (xor (ashr X 'k) (ashr Y 'k)) (ashr (xor X Y) 'k))"),
    Match("(rewrite-if (!and (!ge 'k 0) (!lt 'k 32)) (shl (and (ashr X 'k) 1) 'k) (and X (!shl 1 'k)))"),

    // Canonicalize select into values that later bit rules can see, e.g. select(c, a+b, a) -> a + select(c, b, 0).
    Match("(rewrite (select true  a b) a)"),
    Match("(rewrite (select false a b) b)"),
    Match("(rewrite (select c a a) a)"),
    Match("(rewrite (select c 1 0) c)"),
    Match("(rewrite (select c 0 1) (eq c 0))"),
    Match("(rewrite (select c (add a b) a) (add a (select c b 0)))"),
    Match("(rewrite-if (!and (!ge 'k 0) (!lt 'k 32)) (select (ne (and (ashr X 'k) 1) 0) (!shl 1 'k) 0) (and X (!shl 1 'k)))"),
    Match("(rewrite-if (!and (!ge 'k 0) (!lt 'k 32)) (select (eq (and (ashr X 'k) 1) 1) (!shl 1 'k) 0) (and X (!shl 1 'k)))"),
    Match("(rewrite-if (!and (!ge 'k 0) (!lt 'k 32)) (select (and (ashr X 'k) 1) (!shl 1 'k) 0) (and X (!shl 1 'k)))"),
    Match("(rewrite-if (!pow2 'm) (select (ne (and V 'm) 0) 'm 0) (and V 'm))"),
    Match("(rewrite-if (!pow2 'm) (select (eq (and V 'm) 'm) 'm 0) (and V 'm))"),
    Match("(rewrite-if (!pow2 'm) (select (and V 'm) 'm 0) (and V 'm))"),
};


static bool isConstInt(Value* v, int expected) {
    auto* ci = dyn_cast<ConstantInt>(v);
    return ci && ci->getValue() == expected;
}

static bool matchShiftedValue(Value* v, Value*& base, int& shift) {
    if (auto* bin = dyn_cast<BinaryInst>(v)) {
        if (bin->getOpID() == Instruction::Ashr) {
            auto* ci = dyn_cast<ConstantInt>(bin->getOperand(1));
            if (ci && ci->getValue() >= 0 && ci->getValue() < 32) {
                base = bin->getOperand(0);
                shift = ci->getValue();
                return true;
            }
        }
    }
    base = v;
    shift = 0;
    return true;
}

struct MaskedValue {
    Value* value = nullptr;
    int mask = 0;
};

static bool matchAndConst(Value* v, MaskedValue& out) {
    auto* bin = dyn_cast<BinaryInst>(v);
    if (!bin || bin->getOpID() != Instruction::And)
        return false;

    if (auto* c = dyn_cast<ConstantInt>(bin->getOperand(0))) {
        out = {bin->getOperand(1), c->getValue()};
        return true;
    }
    if (auto* c = dyn_cast<ConstantInt>(bin->getOperand(1))) {
        out = {bin->getOperand(0), c->getValue()};
        return true;
    }
    return false;
}

bool InstSimplify::simplify(BasicBlock* bb) {
    bool changed = false;
    auto* func = bb->getParentFunc();
    std::unique_ptr<ValueTracking> vt;
    if (func)
        vt = std::make_unique<ValueTracking>(func);
    auto isNonNeg = [&](Value* v) -> bool {
        if (auto* ci = dyn_cast<ConstantInt>(v))
            return ci->getValue() >= 0;
        return vt && vt->isNonNeg(v);
    };

    std::vector<Instruction*> worklist(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto inst : worklist) {
        if (!inst->getParent()) continue;

        // icmp x, y -> true/false when Range proves the relation.
        if (auto* cmp = dyn_cast<ICmpInst>(inst)) {
            bool out = false;
            if (vt && vt->knownBool(cmp, out)) {
                cmp->replaceAllUsesWith(new ConstantInt(out ? 1 : 0));
                cmp->eraseInst();
                changed = true;
                continue;
            }
        }

        auto applyRegularMatches = [&](Instruction* inst) {
            for (auto& match : regularMatches) {
                if (match.rewrite(inst))
                    return true;
            }
            return false;
        };

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

        // Rewrite parity inequality through xor only after proving signed operands non-negative.
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

    // Rebuild shifted xor bit-selects into masked values, 
    // select(((X>>s)^(Y>>t))&1,1<<k,0)
    std::vector<Instruction*> wlBitSelect(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto* inst : wlBitSelect) {
        auto* sel = dyn_cast<SelectInst>(inst);
        if (!sel || !sel->getParent()) continue;
        if (!isConstInt(sel->getFalseVal(), 0)) continue;

        auto* trueConst = dyn_cast<ConstantInt>(sel->getTrueVal());
        if (!trueConst) continue;

        auto pow2Shift = [&](int v) {
            if (v <= 0 || (v & (v - 1)) != 0)
                return -1;
            return __builtin_ctz(static_cast<unsigned>(v));
        };

        int outShift = pow2Shift(trueConst->getValue());
        if (outShift < 0 || outShift >= 31) continue;

        auto matchXorBit = [&](Value* cond) -> BinaryInst* {
            auto* andInst = dyn_cast<BinaryInst>(cond);
            if (!andInst || andInst->getOpID() != Instruction::And)
                return nullptr;

            Value* bitVal = nullptr;
            if (isConstInt(andInst->getOperand(0), 1))
                bitVal = andInst->getOperand(1);
            else if (isConstInt(andInst->getOperand(1), 1))
                bitVal = andInst->getOperand(0);
            else
                return nullptr;

            auto* xorInst = dyn_cast<BinaryInst>(bitVal);
            return xorInst && xorInst->getOpID() == Instruction::Xor ? xorInst : nullptr;
        };

        auto* xorBit = matchXorBit(sel->getCond());
        if (!xorBit) continue;

        Value* lhsBase = nullptr;
        Value* rhsBase = nullptr;
        int lhsShift = 0;
        int rhsShift = 0;
        if (!matchShiftedValue(xorBit->getOperand(0), lhsBase, lhsShift)) continue;
        if (!matchShiftedValue(xorBit->getOperand(1), rhsBase, rhsShift)) continue;
        if (lhsShift < outShift || rhsShift < outShift) continue;

        auto makeShift = [&](Value* v, int shift) -> Value* {
            if (shift == 0)
                return v;
            auto* sh = new BinaryInst(Instruction::Ashr, v, new ConstantInt(shift), nullptr);
            sh->setName(">>");
            insertBefore(sel, sh);
            return sh;
        };

        Value* lhs = makeShift(lhsBase, lhsShift - outShift);
        Value* rhs = makeShift(rhsBase, rhsShift - outShift);

        auto* xorInst = new BinaryInst(Instruction::Xor, lhs, rhs, nullptr);
        xorInst->setName("^");
        insertBefore(sel, xorInst);
        auto* andInst = new BinaryInst(Instruction::And, xorInst,
                                    new ConstantInt(trueConst->getValue()), nullptr);
        andInst->setName("&");
        insertBefore(sel, andInst);

        sel->replaceAllUsesWith(andInst);
        sel->eraseInst();
        changed = true;
    }

    // Prove selected bits are zero by walking masks through shifts, ands, constants, and non-negative values.
    auto knownZeroMask = [&](Value* root, uint32_t mask) -> bool {
        std::function<bool(Value*, uint32_t, int)> rec =
            [&](Value* v, uint32_t m, int depth) -> bool {
                if (m == 0) return true;
                if (!v || depth > 16) return false;

                KBits bits = vt ? vt->knownBits(v) : KBits{};
                if ((m & ~bits.zeros) == 0)
                    return true;

                if (isNonNeg(v)) {
                    m &= ~(1u << 31);
                    if (m == 0) return true;
                }

                if (auto* ci = dyn_cast<ConstantInt>(v))
                    return (((uint32_t)ci->getValue()) & m) == 0;

                auto* bin = dyn_cast<BinaryInst>(v);
                if (!bin) return false;

                if (bin->getOpID() == Instruction::And) {
                    MaskedValue mv;
                    if (matchAndConst(bin, mv)) {
                        uint32_t cm = (uint32_t)mv.mask;
                        if ((m & ~cm) == 0)
                            return true;
                        return rec(mv.value, m & cm, depth + 1);
                    }
                }

                auto* sh = dyn_cast<ConstantInt>(bin->getOperand(1));
                if (!sh || sh->getValue() < 0 || sh->getValue() >= 32)
                    return false;

                int k = sh->getValue();
                if (bin->getOpID() == Instruction::Ashr) {
                    uint32_t srcMask = 0;
                    for (int bit = 0; bit < 32; ++bit) {
                        if ((m & (1u << bit)) == 0) continue;
                        int srcBit = std::min(31, bit + k);
                        srcMask |= (1u << srcBit);
                    }
                    return rec(bin->getOperand(0), srcMask, depth + 1);
                }
                if (bin->getOpID() == Instruction::Shl) {
                    uint32_t low = k == 0 ? 0u : ((1u << k) - 1u);
                    m &= ~low;
                    if (m == 0) return true;
                    uint32_t srcMask = 0;
                    for (int bit = k; bit < 32; ++bit) {
                        if (m & (1u << bit))
                            srcMask |= (1u << (bit - k));
                    }
                    return rec(bin->getOperand(0), srcMask, depth + 1);
                }

                return false;
            };
        return rec(root, mask, 0);
    };

    auto buildAnd = [&](Instruction* before, Value* value, int mask) -> BinaryInst* {
        auto* andInst = new BinaryInst(Instruction::And, value, new ConstantInt(mask), nullptr);
        andInst->setName(before->getName());
        insertBefore(before, andInst);
        return andInst;
    };

    // Drop redundant masks when all cleared bits are already known zero.
    std::vector<Instruction*> wlRedundantAnd(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto* inst : wlRedundantAnd) {
        if (!inst->getParent()) continue;
        MaskedValue mv;
        if (!matchAndConst(inst, mv)) continue;
        if (knownZeroMask(mv.value, (uint32_t)mv.mask)) {
            inst->replaceAllUsesWith(new ConstantInt(0));
            inst->eraseInst();
            changed = true;
            continue;
        }
        uint32_t cleared = ~((uint32_t)mv.mask);
        if (!knownZeroMask(mv.value, cleared)) continue;
        inst->replaceAllUsesWith(mv.value);
        inst->eraseInst();
        changed = true;
    }

    // Remove zero xor operands under a mask, 
    // (xor A B)&m -> B&m when A&m is zero.
    std::vector<Instruction*> wlMaskedXor(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto* inst : wlMaskedXor) {
        if (!inst->getParent()) continue;
        MaskedValue mv;
        if (!matchAndConst(inst, mv)) continue;
        auto* xorInst = dyn_cast<BinaryInst>(mv.value);
        if (!xorInst || xorInst->getOpID() != Instruction::Xor) continue;

        Value* lhs = xorInst->getOperand(0);
        Value* rhs = xorInst->getOperand(1);
        uint32_t mask = (uint32_t)mv.mask;
        Value* replacementBase = nullptr;
        if (knownZeroMask(lhs, mask))
            replacementBase = rhs;
        else if (knownZeroMask(rhs, mask))
            replacementBase = lhs;
        else
            continue;

        auto* andInst = buildAnd(inst, replacementBase, mv.mask);
        inst->replaceAllUsesWith(andInst);
        inst->eraseInst();
        changed = true;
    }

    // Absorb adjacent masked pieces into a wider masked xor when the other operand is zero on the new bits.
    std::vector<Instruction*> wlOrAbsorb(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto* inst : wlOrAbsorb) {
        auto* bin = dyn_cast<BinaryInst>(inst);
        if (!bin || !bin->getParent() || bin->getOpID() != Instruction::Or)
            continue;

        MaskedValue L, R;
        if (!matchAndConst(bin->getOperand(0), L) || !matchAndConst(bin->getOperand(1), R))
            continue;

        auto tryAbsorb = [&](const MaskedValue& XorSide, const MaskedValue& OtherSide) -> BinaryInst* {
            auto* xorInst = dyn_cast<BinaryInst>(XorSide.value);
            if (!xorInst || xorInst->getOpID() != Instruction::Xor)
                return nullptr;
            Value* lhs = xorInst->getOperand(0);
            Value* rhs = xorInst->getOperand(1);
            uint32_t otherMask = (uint32_t)OtherSide.mask;

            if (OtherSide.value == lhs && knownZeroMask(rhs, otherMask))
                return buildAnd(bin, xorInst, XorSide.mask | OtherSide.mask);
            if (OtherSide.value == rhs && knownZeroMask(lhs, otherMask))
                return buildAnd(bin, xorInst, XorSide.mask | OtherSide.mask);
            return nullptr;
        };

        BinaryInst* replacement = tryAbsorb(L, R);
        if (!replacement)
            replacement = tryAbsorb(R, L);
        if (!replacement)
            continue;

        bin->replaceAllUsesWith(replacement);
        bin->eraseInst();
        changed = true;
    }

    // Convert add to or when KnownBits proves the operands cannot carry into each other.
    std::vector<Instruction*> wl3(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto* inst : wl3) {
        if (!inst->getParent()) continue;
        auto* bin = dyn_cast<BinaryInst>(inst);
        if (!bin || bin->getOpID() != Instruction::Add) continue;
        KBits LA = vt ? vt->knownBits(bin->getOperand(0)) : KBits{};
        KBits RA = vt ? vt->knownBits(bin->getOperand(1)) : KBits{};
        if (!LA.disjointWith(RA)) continue;
        auto& il = bb->getInstructions();
        auto pos = std::find(il.begin(), il.end(), bin);
        auto* orInst = new BinaryInst(Instruction::Or,
                                    bin->getOperand(0), bin->getOperand(1), nullptr);
        orInst->setName(bin->getName());
        orInst->setParent(bb);
        il.insert(pos, orInst);
        bin->replaceAllUsesWith(orInst);
        bin->eraseInst();
        changed = true;
    }

    return changed;
}
