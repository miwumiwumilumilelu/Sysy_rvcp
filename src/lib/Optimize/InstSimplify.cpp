#include "Optimize/InstSimplify.h"

using namespace sysy;

bool InstSimplify::run() {
    bool anyChanged = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto func : TheModule->getFunctions()) {
            for (auto bb : func->getBody()->getBlocks()) {
                if (simplify(bb)) { changed = true; anyChanged = true; }
            }
        }
    }
    return anyChanged;
}

static bool replaceTo(Instruction* inst, BasicBlock* bb, Value* v) {
    inst->replaceAllUsesWith(v);
    bb->getInstructions().remove(inst);
    return true;
}

// If one of the operands is a constant which is either 0 or 1, simplify the BinaryInst.
bool InstSimplify::simplify(BasicBlock* bb) {
    bool changed = false;
    std::vector<Instruction*> worklist(bb->getInstructions().begin(), bb->getInstructions().end());
    for (auto inst : worklist) {
        auto bin = dyn_cast<BinaryInst>(inst);
        if (!bin) continue;

        Value* lhs = bin->getOperand(0);
        Value* rhs = bin->getOperand(1);
        auto ci_l = dyn_cast<ConstantInt>(lhs);
        auto ci_r = dyn_cast<ConstantInt>(rhs);
        auto cf_l = dyn_cast<ConstantFloat>(lhs);
        auto cf_r = dyn_cast<ConstantFloat>(rhs);

        switch (bin->getOpID()) {
            case Instruction::Add:
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (ci_l && ci_l->getValue() == 0) { changed |= replaceTo(inst, bb, rhs); continue; }
                if (cf_r && cf_r->getValue() == 0.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_l && cf_l->getValue() == 0.0f) { changed |= replaceTo(inst, bb, rhs); continue; }
                break;
            case Instruction::Sub:
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_r && cf_r->getValue() == 0.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (lhs == rhs) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                break;
            case Instruction::Mul:
                if (ci_r && ci_r->getValue() == 1) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (ci_l && ci_l->getValue() == 1) { changed |= replaceTo(inst, bb, rhs); continue; }
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                if (ci_l && ci_l->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                if (cf_r && cf_r->getValue() == 1.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_l && cf_l->getValue() == 1.0f) { changed |= replaceTo(inst, bb, rhs); continue; }
                if (cf_r && cf_r->getValue() == 0.0f) { changed |= replaceTo(inst, bb, new ConstantFloat(0.0f)); continue; }
                if (cf_l && cf_l->getValue() == 0.0f) { changed |= replaceTo(inst, bb, new ConstantFloat(0.0f)); continue; }
                break;
            case Instruction::Div:
                if (ci_r && ci_r->getValue() == 1) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_r && cf_r->getValue() == 1.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (ci_l && ci_l->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                if (cf_l && cf_l->getValue() == 0.0f) { changed |= replaceTo(inst, bb, new ConstantFloat(0.0f)); continue; }
                break;
            case Instruction::Mod:
                if (ci_r && ci_r->getValue() == 1)   { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                break;
            default:
                break;
        }
    }
    return changed;
}
