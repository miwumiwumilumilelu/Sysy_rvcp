#include "Optimize/Scalar/InstSimplify.h"
#include <algorithm>

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

static bool replaceTo(Instruction* inst, BasicBlock* bb, Value* v) {
    inst->replaceAllUsesWith(v);
    bb->getInstructions().remove(inst);
    return true;
}

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
                if (ci_l && !ci_r) { bin->setOperand(0, rhs); bin->setOperand(1, lhs); std::swap(ci_l, ci_r); std::swap(lhs, rhs);}
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_l && !cf_r) { bin->setOperand(0, rhs); bin->setOperand(1, lhs); std::swap(cf_l, cf_r); std::swap(lhs, rhs);}
                if (cf_r && cf_r->getValue() == 0.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                break;
            case Instruction::Sub:
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_r && cf_r->getValue() == 0.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (lhs == rhs) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                break;
            case Instruction::Mul:
                if (ci_l && !ci_r) { bin->setOperand(0, rhs); bin->setOperand(1, lhs); std::swap(ci_l, ci_r); std::swap(lhs, rhs);}
                if (ci_r && ci_r->getValue() == 1) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                if (cf_l && !cf_r) { bin->setOperand(0, rhs); bin->setOperand(1, lhs); std::swap(cf_l, cf_r); std::swap(lhs, rhs);}
                if (cf_r && cf_r->getValue() == 1.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_r && cf_r->getValue() == 0.0f) { changed |= replaceTo(inst, bb, new ConstantFloat(0.0f)); continue; }
                break;
            case Instruction::Div:
                if (ci_r && ci_r->getValue() == 1) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (cf_r && cf_r->getValue() == 1.0f) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (ci_l && ci_l->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                if (cf_l && cf_l->getValue() == 0.0f) { changed |= replaceTo(inst, bb, new ConstantFloat(0.0f)); continue; }
                break;
            case Instruction::Mod:
                if (ci_r && ci_r->getValue() == 1) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                break;
            case Instruction::Shl:
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (ci_l && ci_l->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                if (ci_r && ci_r->getValue() >= 0) {
                    if (auto* inner = dyn_cast<BinaryInst>(lhs)) {
                        // Shl(Shl(x, c1), c2) -> Shl(x, c1+c2), but if c1+c2 >= 32 then it's 0.
                        if (inner->getOpID() == Instruction::Shl) {
                            if (auto* ic = dyn_cast<ConstantInt>(inner->getOperand(1))) {
                                if (ic->getValue() >= 0 &&
                                    ci_r->getValue() + ic->getValue() >= 32) {
                                    changed |= replaceTo(inst, bb, new ConstantInt(0));
                                    continue;
                                }
                            }
                        }
                    }
                }
                break;
            case Instruction::Ashr:
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (ci_l && ci_l->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                break;
            case Instruction::And:
                if (ci_l && !ci_r) { bin->setOperand(0, rhs); bin->setOperand(1, lhs); std::swap(ci_l, ci_r); std::swap(lhs, rhs);}
                if (ci_r && ci_r->getValue() == 0) { changed |= replaceTo(inst, bb, new ConstantInt(0)); continue; }
                if (ci_r && ci_r->getValue() == -1) { changed |= replaceTo(inst, bb, lhs); continue; }
                if (lhs == rhs) { changed |= replaceTo(inst, bb, lhs); continue; }
                break;
            default:
                break;
        }
    }

    // shl(shl(x, c1), c2) -> shl(x, c1+c2)
    // ashr(ashr(x, c1), c2) -> ashr(x, min(c1+c2, 31))
    // and(and(x, m1), m2) -> and(x, m1 & m2)
    {
        struct Rewrite { 
            Instruction* old_inst; 
            Instruction* new_inst; 
        };
        std::vector<Rewrite> rewrites;

        for (auto* inst : bb->getInstructions()) {
            auto* outer = dyn_cast<BinaryInst>(inst);
            if (!outer) continue;
            auto op = outer->getOpID();
            if (op != Instruction::Shl &&
                op != Instruction::Ashr &&
                op != Instruction::And) continue;

            Value* lhs = outer->getOperand(0);
            auto* ci_r = dyn_cast<ConstantInt>(outer->getOperand(1));
            if (!ci_r) continue;

            auto* inner = dyn_cast<BinaryInst>(lhs);
            if (!inner || inner->getOpID() != op) continue;

            auto* ci_r2 = dyn_cast<ConstantInt>(inner->getOperand(1));
            if (!ci_r2) continue;

            int c1 = ci_r2->getValue();
            int c2 = ci_r->getValue();

            if (c1 < 0 || c2 < 0) continue;

            Value* x = inner->getOperand(0);
            int c;
            if (op == Instruction::Shl) {
                c = c1 + c2;
                if (c >= 32) continue;
            } else if (op == Instruction::Ashr) {
                c = std::min(c1 + c2, 31);
            } else {
                c = c1 & c2;
            }

            auto* newInst = new BinaryInst(op, x, new ConstantInt(c), nullptr);
            newInst->setParent(bb);
            rewrites.push_back({inst, newInst});
        }

        for (auto& rw : rewrites) {
            auto& instList = bb->getInstructions();
            auto it = std::find(instList.begin(), instList.end(), rw.old_inst);
            instList.insert(it, rw.new_inst);
            rw.old_inst->replaceAllUsesWith(rw.new_inst);
            instList.erase(it);
            changed = true;
        }
    }

    return changed;
}
