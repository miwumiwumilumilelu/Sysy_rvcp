#include "../../../include/Optimize/Scalar/StrengthReduce.h"
#include "../../../include/Optimize/Analysis/ValueTracking.h"
#include "../../../include/IR/Module.h"
#include "../../../include/IR/Instruction.h"
#include "../../../include/IR/Value.h"
#include <algorithm>

using namespace sysy;

static int log2v(int v) {
    if (v > 1 && (v & (v - 1)) == 0)
        return __builtin_ctz(static_cast<unsigned>(v));
    return -1;
}

bool StrengthReduce::rewriteFunc(Function* f, ValueTracking& vt) {
    bool changed = false;

    auto isNonNeg = [&](Value* v) -> bool {
        return vt.isNonNeg(v);
    };

    auto isZero = [&](Value* v) -> bool {
        IRange r = vt.range(v);
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
        rw.old_inst->eraseInst();
        changed = true;
    }

    return changed;
}

bool StrengthReduce::run() {
    bool any = false;
    for (auto* f : M->getFunctions()) {
        if (f->getBody()->getBlocks().empty()) continue;
        ValueTracking vt(f);
        any |= rewriteFunc(f, vt);
    }
    return any;
}
