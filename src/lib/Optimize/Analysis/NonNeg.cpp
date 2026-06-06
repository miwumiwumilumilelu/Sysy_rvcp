#include "Optimize/Analysis/NonNeg.h"
#include "Optimize/Analysis/Range.h"
#include "IR/Instruction.h"

using namespace sysy;

NonNegAnalysis::NonNegAnalysis(Function* f, const RangeAnalysis* RA) : F(f), ExtRA(RA) {
    analyze();
}

bool NonNegAnalysis::isNonNeg(Value* v) const {
    if (auto* ci = dyn_cast<ConstantInt>(v))
        return ci->getValue() >= 0;
    return S.count(v) > 0;
}

const RangeAnalysis& NonNegAnalysis::ranges() {
    if (ExtRA)
        return *ExtRA;
    if (!OwnRA)
        OwnRA = std::make_unique<RangeAnalysis>(F);
    return *OwnRA;
}

void NonNegAnalysis::analyze() {
    if (!F || F->getBody()->getBlocks().empty())
        return;

    // init
    for (auto* bb : F->getBody()->getBlocks())
        for (auto* inst : bb->getInstructions())
            S.insert(inst);

    const RangeAnalysis& RA = ranges();

    auto justified = [&](Value* v) -> bool {
        if (RA.isNonNeg(v))
            return true;

        if (auto* bin = dyn_cast<BinaryInst>(v)) {
            Value* lhs = bin->getOperand(0);
            Value* rhs = bin->getOperand(1);
            switch (bin->getOpID()) {
                case Instruction::Ashr:
                    return isNonNeg(lhs);
                case Instruction::And:
                    return isNonNeg(lhs) || isNonNeg(rhs);
                case Instruction::Or:
                case Instruction::Xor:
                    return isNonNeg(lhs) && isNonNeg(rhs);
                case Instruction::Add:
                case Instruction::Mul:
                    return isNonNeg(lhs) && isNonNeg(rhs);
                case Instruction::Mod:
                    return isNonNeg(lhs);
                case Instruction::Div:
                    return isNonNeg(lhs) && RA.isPositive(rhs);
                default:
                    return false;
            }
        }

        if (auto* phi = dyn_cast<PhiInst>(v)) {
            int n = phi->getNumOperands();
            if (n == 0) return false;
            for (int i = 0; i < n; i += 2)
                if (!isNonNeg(phi->getOperand(i))) return false;
            return true;
        }

        if (auto* sel = dyn_cast<SelectInst>(v))
            return isNonNeg(sel->getTrueVal()) && isNonNeg(sel->getFalseVal());

        // 0 or 1
        return isa<ICmpInst>(v) || isa<FCmpInst>(v);
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = S.begin(); it != S.end(); ) {
            if (!justified(*it)) {
                it = S.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
}
