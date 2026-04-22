#include "Optimize/Scalar/StrengthReduce.h"
#include "Optimize/Analysis/Range.h"
#include "IR/Module.h"
#include "IR/Instruction.h"
#include "IR/Value.h"
#include <algorithm>

using namespace sysy;

static int log2v(int v) {
    if (v > 1 && (v & (v - 1)) == 0)
        return __builtin_ctz(static_cast<unsigned>(v));
    return -1;
}

// Seed every instruction as non-negative, then prune those that cannot be justified.  
// Handles cyclic phi nodes correctly.
void StrengthReduce::computeNonNeg() {
    for (auto* f : M->getFunctions()) {
        if (f->getBody()->getBlocks().empty()) continue;

        auto& nn = nonNegMap[f];
        nn.clear();

        for (auto* bb : f->getBody()->getBlocks())
            for (auto* inst : bb->getInstructions())
                nn.insert(inst);

        auto isNonnegVal = [&](Value* v) -> bool {
            if (auto* ci = dyn_cast<ConstantInt>(v)) return ci->getValue() >= 0;
            return nn.count(v) > 0;
        };

        auto isJustified = [&](Value* v) -> bool {
            if (auto* bin = dyn_cast<BinaryInst>(v)) {
                Value* l = bin->getOperand(0);
                Value* r = bin->getOperand(1);
                switch (bin->getOpID()) {
                    case Instruction::Ashr:
                        return isNonnegVal(l); // sign bit propagates
                    case Instruction::And:
                        return isNonnegVal(l) || isNonnegVal(r); // clears sign bit
                    case Instruction::Add:
                    case Instruction::Mul:
                        return isNonnegVal(l) && isNonnegVal(r);
                    case Instruction::Div:
                    case Instruction::Mod:
                        return isNonnegVal(l);
                    default:
                        return false;
                }
            }
            if (auto* phi = dyn_cast<PhiInst>(v)) {
                int n = phi->getNumOperands();
                if (n == 0) return false;
                for (int i = 0; i < n; i += 2)
                    if (!isNonnegVal(phi->getOperand(i))) return false;
                return true;
            }
            return false;
        };

        bool pruneChanged = true;
        while (pruneChanged) {
            pruneChanged = false;
            std::vector<Value*> toRemove;
            for (auto* v : nn)
                if (!isJustified(v)) toRemove.push_back(v);
            for (auto* v : toRemove) {
                nn.erase(v);
                pruneChanged = true;
            }
        }
    }
}

bool StrengthReduce::rewriteFunc(Function* f, const NonNegSet& nonneg) {
    bool changed = false;

    RangeAnalysis ra(f);

    auto isNonNeg = [&](Value* v) -> bool {
        return nonneg.count(v) > 0 || ra.isNonNeg(v);
    };

    auto isZero = [&](Value* v) -> bool {
        IRange r = ra.get(v);
        return r.low == 0 && r.high == 0;
    };

    struct Rewrite {
        Instruction* old_inst;
        Value*       new_val;
    };
    std::vector<Rewrite> rewrites;

    for (auto* bb : f->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            auto* bin = dyn_cast<BinaryInst>(inst);
            if (!bin) continue;

            auto op = bin->getOpID();
            if (op != Instruction::Mul &&
                op != Instruction::Div &&
                op != Instruction::Mod) continue;

            Value* lhs = bin->getOperand(0);
            Value* rhs = bin->getOperand(1);
            auto* ci = dyn_cast<ConstantInt>(rhs);
            if (!ci) continue;

            int v = ci->getValue();
            int k = log2v(v);
            if (k < 0) continue;

            Value* newVal = nullptr;
            if (isZero(lhs)) {
                newVal = new ConstantInt(0);
            } else if (op == Instruction::Mul) {
                newVal = new BinaryInst(Instruction::Shl, lhs,
                                        new ConstantInt(k), nullptr);
            } else if (op == Instruction::Div) {
                if (!isNonNeg(lhs)) continue;
                newVal = new BinaryInst(Instruction::Ashr, lhs,
                                        new ConstantInt(k), nullptr);
            } else {
                if (!isNonNeg(lhs)) continue;
                newVal = new BinaryInst(Instruction::And, lhs,
                                        new ConstantInt(v - 1), nullptr);
            }

            if (auto* newInst = dyn_cast<Instruction>(newVal))
                newInst->setParent(bb);
            rewrites.push_back({bin, newVal});
        }
    }

    for (auto& rw : rewrites) {
        auto* bb = rw.old_inst->getParent();
        auto& instList = bb->getInstructions();
        auto it = std::find(instList.begin(), instList.end(), rw.old_inst);
        if (auto* newInst = dyn_cast<Instruction>(rw.new_val)) {
            if (!rw.old_inst->getName().empty())
                newInst->setName(rw.old_inst->getName());
            instList.insert(it, newInst);
        }
        rw.old_inst->replaceAllUsesWith(rw.new_val);
        instList.erase(it);
        changed = true;
    }

    return changed;
}

bool StrengthReduce::run() {
    computeNonNeg();
    bool any = false;
    for (auto* f : M->getFunctions()) {
        if (f->getBody()->getBlocks().empty()) continue;
        any |= rewriteFunc(f, nonNegMap[f]);
    }
    return any;
}
