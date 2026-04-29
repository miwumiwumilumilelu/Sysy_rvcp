#include "Optimize/Analysis/Range.h"
#include "IR/Instruction.h"
#include "IR/Module.h"
#include <algorithm>
#include <climits>
#include <cstdlib>

using namespace sysy;

IRange sysy::rangeJoin(IRange l, IRange r, bool widen) {
    if (widen)
        return { r.low < l.low ? INT_MIN : l.low,
                 l.high < r.high ? INT_MAX : l.high };
    return { std::min(l.low, r.low), std::max(l.high, r.high) };
}

IRange sysy::rangeMeet(IRange l, IRange r) {
    int lo = std::max(l.low, r.low);
    int hi = std::min(l.high, r.high);
    return lo <= hi ? IRange{lo, hi} : IRange::unknown();
}

static int clamp(int64_t x) {
    if (x > INT_MAX) return INT_MAX;
    if (x < INT_MIN) return INT_MIN;
    return static_cast<int>(x);
}

static int64_t minmul(int a1, int b1, int a2, int b2) {
    __int128 xs[] = {
        (__int128)a1 * a2, (__int128)a1 * b2,
        (__int128)b1 * a2, (__int128)b1 * b2
    };
    __int128 m = *std::min_element(xs, xs + 4);
    return m < INT_MIN ? (int64_t)INT_MIN : (int64_t)m;
}

static int64_t maxmul(int a1, int b1, int a2, int b2) {
    __int128 xs[] = {
        (__int128)a1 * a2, (__int128)a1 * b2,
        (__int128)b1 * a2, (__int128)b1 * b2
    };
    __int128 m = *std::max_element(xs, xs + 4);
    return m > INT_MAX ? (int64_t)INT_MAX : (int64_t)m;
}

static int64_t mindiv(int64_t a1, int64_t b1, int64_t a2, int64_t b2) {
    if (a2 == 0 || b2 == 0) return INT_MIN;
    if (a2 * b2 < 0)
        return -std::max(std::abs(b1), std::abs(b2));
    int64_t xs[] = { a1/a2, a1/b2, b1/a2, b1/b2 };
    return *std::min_element(xs, xs + 4);
}

static int64_t maxdiv(int64_t a1, int64_t b1, int64_t a2, int64_t b2) {
    if (a2 == 0 || b2 == 0) return INT_MAX;
    if (a2 * b2 < 0)
        return std::max(std::abs(a1), std::abs(b1));
    int64_t xs[] = { a1/a2, a1/b2, b1/a2, b1/b2 };
    return *std::max_element(xs, xs + 4);
}

static int minmod(int a1, int b1, int a2, int b2) {
    if (a1 >= 0 && a2 > 0)  return 0;
    if (a1 >= 0 && a2 > b1) return a1;
    return -std::max(std::abs(a2), std::abs(b2)) + 1;
}

static int maxmod(int a1, int b1, int a2, int b2) {
    if (a1 >= 0 && a2 > b1) return b1;
    return std::max(std::abs(a2), std::abs(b2)) - 1;
}

// Replace every occurrence of oldVal as an operand of user with newVal.
static void replaceInUser(User* user, Value* oldVal, Value* newVal) {
    for (int i = 0; i < user->getNumOperands(); ++i)
        if (user->getOperand(i) == oldVal)
            user->setOperand(i, newVal);
}

bool RangeAnalysis::hasRange(Value* v) const {
    if (dyn_cast<ConstantInt>(v)) return true;
    return ranges.count(v) > 0;
}

IRange RangeAnalysis::getRange(Value* v) const {
    if (auto* ci = dyn_cast<ConstantInt>(v))
        return IRange::scalar(ci->getValue());
    auto it = ranges.find(v);
    return it != ranges.end() ? it->second : IRange::unknown();
}

bool RangeAnalysis::has(Value* v)  const { return hasRange(v); }
IRange RangeAnalysis::get(Value* v) const { return getRange(v); }

bool RangeAnalysis::isNonNeg(Value* v) const {
    if (auto* ci = dyn_cast<ConstantInt>(v)) return ci->getValue() >= 0;
    auto it = ranges.find(v);
    return it != ranges.end() && it->second.low >= 0;
}

bool RangeAnalysis::isPositive(Value* v) const {
    if (auto* ci = dyn_cast<ConstantInt>(v)) return ci->getValue() > 0;
    auto it = ranges.find(v);
    return it != ranges.end() && it->second.low > 0;
}

bool RangeAnalysis::narrowConditional(PhiInst* phi, bool& changed) {
    // Split phis have exactly 1 incoming -> 2 operands [value, BasicBlock].
    if (phi->getNumOperands() != 2) return false;

    Value* x = phi->getOperand(0);
    auto* predBB = dyn_cast<BasicBlock>(phi->getOperand(1));
    if (!predBB || !hasRange(x)) return false;

    // The predecessor must end with a conditional branch.
    auto& insts = predBB->getInstructions();
    if (insts.empty()) return false;
    auto* term = dyn_cast<BranchInst>(insts.back());
    if (!term || term->getNumOperands() != 3) return false;

    auto* cond = dyn_cast<ICmpInst>(term->getOperand(0));
    if (!cond) return false;

    BasicBlock* thisBB = phi->getParent();
    auto* trueBB = dyn_cast<BasicBlock>(term->getOperand(1));
    auto* falseBB = dyn_cast<BasicBlock>(term->getOperand(2));
    bool isTrue = (thisBB == trueBB);
    bool isFalse = (thisBB == falseBB);
    if (!isTrue && !isFalse) return false;

    Value* lhs = cond->getOperand(0);
    Value* rhs = cond->getOperand(1);

    // x must appear on one side of the comparison.
    if (lhs != x && rhs != x) return false;
    if (!hasRange(lhs) || !hasRange(rhs)) return false;

    ICmpInst::CmpOp pred = cond->getPredicate();

    // transpose the predicate if x is on the rhs.
    if (rhs == x) {
        switch (pred) {
            case ICmpInst::SLT: pred = ICmpInst::SGT; break;
            case ICmpInst::SLE: pred = ICmpInst::SGE; break;
            case ICmpInst::SGT: pred = ICmpInst::SLT; break;
            case ICmpInst::SGE: pred = ICmpInst::SLE; break;
            default: break;
        }
        std::swap(lhs, rhs);
    }

    // Current range for x and the other operand.
    IRange xr = getRange(x);
    IRange or_ = getRange(rhs);
    int lo = xr.low, hi = xr.high;
    int olo = or_.low, ohi = or_.high;

    // Flip predicate for the false edge.
    if (!isTrue) {
        switch (pred) {
            case ICmpInst::SLT: pred = ICmpInst::SGE; break;
            case ICmpInst::SLE: pred = ICmpInst::SGT; break;
            case ICmpInst::SGT: pred = ICmpInst::SLE; break;
            case ICmpInst::SGE: pred = ICmpInst::SLT; break;
            case ICmpInst::EQ: pred = ICmpInst::NE; break;
            case ICmpInst::NE: pred = ICmpInst::EQ; break;
        }
    }

    switch (pred) {
        case ICmpInst::SLT: // x < other
            if (ohi != INT_MAX) hi = std::min(hi, ohi - 1);
            break;
        case ICmpInst::SLE: // x <= other
            hi = std::min(hi, ohi);
            break;
        case ICmpInst::SGT: // x > other
            if (olo != INT_MIN) lo = std::max(lo, olo + 1);
            break;
        case ICmpInst::SGE: // x >= other
            lo = std::max(lo, olo);
            break;
        case ICmpInst::EQ: // x == other
            lo = std::max(lo, olo);
            hi = std::min(hi, ohi);
            break;
        case ICmpInst::NE:
            break;
    }

    if (lo > hi) return true;

    IRange newRange{lo, hi};
    if (!hasRange(phi) || getRange(phi) != newRange) {
        ranges[phi] = newRange;
        changed = true;
    }
    return true;
}

bool RangeAnalysis::calculateRange(Instruction* inst, int nowiden) {
    // Only track integer-typed instructions.
    if (!inst->getType()->isInt()) return false;

    if (auto* phi = dyn_cast<PhiInst>(inst)) {
        bool changed = false;
        bool isSplitPhi = narrowConditional(phi, changed);
        if (changed) return true;
        if (isSplitPhi) return false;

        // Normal phi: join all incoming values.
        int lo = INT_MAX, hi = INT_MIN;
        bool anyKnown = false;
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            Value* val = phi->getOperand(i);
            if (!val || !hasRange(val)) continue;
            IRange r = getRange(val);
            lo = std::min(lo, r.low);
            hi = std::max(hi, r.high);
            anyKnown = true;
        }
        if (!anyKnown) return false;

        IRange r{lo, hi};
        if (hasRange(inst)) {
            IRange cur = getRange(inst);
            if (cur == r) return false;
            IRange joined = rangeJoin(cur, r, nowiden <= 0);
            if (joined == cur) return false;
            ranges[inst] = joined;
            return true;
        }
        ranges[inst] = r;
        return true;
    }

    if (auto* bin = dyn_cast<BinaryInst>(inst)) {
        Value* lhs = bin->getOperand(0);
        Value* rhs = bin->getOperand(1);
        if (!hasRange(lhs) || !hasRange(rhs)) {
            if (!hasRange(bin)) { ranges[bin] = IRange::unknown(); return true; }
            return false;
        }
        auto [a1, b1] = getRange(lhs);
        auto [a2, b2] = getRange(rhs);

        IRange r = IRange::unknown();
        switch (bin->getOpID()) {
            case Instruction::Add:
                r = { clamp((int64_t)a1 + a2), clamp((int64_t)b1 + b2) };
                break;
            case Instruction::Sub:
                r = { clamp((int64_t)a1 - b2), clamp((int64_t)b1 - a2) };
                break;
            case Instruction::Mul:
                r = { clamp(minmul(a1,b1,a2,b2)), clamp(maxmul(a1,b1,a2,b2)) };
                break;
            case Instruction::Div:
                r = { clamp(mindiv(a1,b1,a2,b2)), clamp(maxdiv(a1,b1,a2,b2)) };
                break;
            case Instruction::Mod:
                r = { minmod(a1,b1,a2,b2), maxmod(a1,b1,a2,b2) };
                break;
            case Instruction::Ashr: {
                // Arithmetic right shift preserves sign: [lo>>k, hi>>k].
                auto* ci = dyn_cast<ConstantInt>(rhs);
                if (ci && ci->getValue() >= 0 && ci->getValue() < 32) {
                    int k = ci->getValue();
                    r = { a1 >> k, b1 >> k };
                }
                break;
            }
            case Instruction::Shl: {
                // only tractable with a constant non-negative shift.
                auto* ci = dyn_cast<ConstantInt>(rhs);
                if (ci && ci->getValue() >= 0 && ci->getValue() < 32 && a1 >= 0) {
                    int k = ci->getValue();
                    r = { clamp((int64_t)a1 << k), clamp((int64_t)b1 << k) };
                }
                break;
            }
            case Instruction::And: {
                // AND with a non-negative constant mask -> result in [0, mask].
                auto* ci = dyn_cast<ConstantInt>(rhs);
                if (!ci) ci = dyn_cast<ConstantInt>(lhs);
                if (ci && ci->getValue() >= 0) {
                    int mask = ci->getValue();
                    r = { 0, mask };
                }
                break;
            }
            default:
                break;
        }

        if (hasRange(bin)) {
            IRange cur = getRange(bin);
            IRange joined = rangeJoin(cur, r, false);
            if (joined == cur) return false;
            ranges[bin] = joined;
            return true;
        }
        ranges[bin] = r;
        return true;
    }

    if (dyn_cast<ICmpInst>(inst)) {
        if (hasRange(inst)) return false;
        ranges[inst] = {0, 1};
        return true;
    }

    if (!hasRange(inst)) {
        ranges[inst] = IRange::unknown();
        return true;
    }
    return false;
}

void RangeAnalysis::doSplit() {
    auto hasSplitPhi = [](BasicBlock* bb, Value* x, BasicBlock* fromBB) -> bool {
        for (auto* inst : bb->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) continue;
            if (phi->getNumOperands() == 2 &&
                phi->getOperand(0) == x &&
                phi->getOperand(1) == fromBB)
                return true;
        }
        return false;
    };

    for (auto* bb : F->getBody()->getBlocks()) {
        auto& insts = bb->getInstructions();
        if (insts.empty()) continue;

        // Block must end with a conditional branch.
        auto* term = dyn_cast<BranchInst>(insts.back());
        if (!term || term->getNumOperands() != 3) continue;

        // Condition must be an integer comparison.
        auto* cond = dyn_cast<ICmpInst>(term->getOperand(0));
        if (!cond) continue;

        // We split on the LHS of the comparison.
        Value* x = cond->getOperand(0);
        if (!x->getType()->isInt()) continue;
        if (dyn_cast<ConstantInt>(x)) continue;

        auto* bb1 = dyn_cast<BasicBlock>(term->getOperand(1)); // true successor
        auto* bb2 = dyn_cast<BasicBlock>(term->getOperand(2)); // false successor
        if (!bb1 || !bb2) continue;

        // Avoid self-loops (single-block rotated loops).
        if (bb1 == bb || bb2 == bb) continue;

        // bb must dominate both successors (excludes loop latches).
        if (!dom.dominates(bb, bb1) || !dom.dominates(bb, bb2)) continue;

        // Don't insert duplicate split-phis.
        if (hasSplitPhi(bb1, x, bb) && hasSplitPhi(bb2, x, bb)) continue;

        // Insert x1 = phi [x from bb] at the front of bb1.
        auto* x1 = new PhiInst(x->getType(), bb1);
        x1->addIncoming(x, bb);
        bb1->getInstructions().push_front(x1);
        splitPhis_.push_back(x1);

        // Insert x2 = phi [x from bb] at the front of bb2.
        auto* x2 = new PhiInst(x->getType(), bb2);
        x2->addIncoming(x, bb);
        bb2->getInstructions().push_front(x2);
        splitPhis_.push_back(x2);

        // Rename uses of x in each successor's dominated subtree.
        // Take a snapshot because setOperand modifies the UseList.
        std::vector<User*> users(x->getUsers().begin(), x->getUsers().end());
        for (auto* user : users) {
            if (user == x1 || user == x2) continue;
            auto* useInst = dyn_cast<Instruction>(user);
            if (!useInst) continue;
            BasicBlock* useBB = useInst->getParent();
            if (dom.dominates(bb1, useBB))
                replaceInUser(user, x, x1);
            if (dom.dominates(bb2, useBB))
                replaceInUser(user, x, x2);
        }
    }
}

void RangeAnalysis::undoSplit() {
    for (auto* sp : splitPhis_) {
        // Restore all users to use origVal.
        Value* origVal = sp->getOperand(0);
        sp->replaceAllUsesWith(origVal);
        ranges.erase(sp);
        sp->eraseInst();
    }
    splitPhis_.clear();
}

void RangeAnalysis::doAnalyze() {
    // Initialize argument ranges to unknown.
    for (auto* arg : F->getArgs())
        if (arg->getType()->isInt())
            ranges[arg] = IRange::unknown();

    int nowiden = 4;
    bool changed;
    do {
        changed = false;
        --nowiden;
        for (auto* bb : F->getBody()->getBlocks())
            for (auto* inst : bb->getInstructions())
                changed |= calculateRange(inst, nowiden);
    } while (changed);
}

RangeAnalysis::RangeAnalysis(Function* f) : F(f), dom(f) {
    dom.run();
    doSplit();
    doAnalyze();
    undoSplit();
}
