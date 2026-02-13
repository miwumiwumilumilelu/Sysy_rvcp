#include "Optimize/ConstantFold.h"
#include <cmath>

using namespace sysy;

void ConstantFold::run() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto func : TheModule->getFunctions()) {
            for (auto bb : func->getBody()->getBlocks()) {
                std::vector<Instruction*> worklist;
                for (auto inst : bb->getInstructions()) {
                    worklist.push_back(inst);
                }

                for (auto inst : worklist) {
                    if (foldInstruction(inst)) {
                        changed = true;
                    }
                }
            }
        }
    }
}

bool ConstantFold::foldInstruction(Instruction* inst) {
    if (auto bin = dyn_cast<BinaryInst>(inst)) {
        auto c1 = dyn_cast<Constant>(bin->getOperand(0));
        auto c2 = dyn_cast<Constant>(bin->getOperand(1));

        if (c1 && c2) {
            if (auto folded = computeBinary(bin->getOpID(), c1, c2)) {
                inst->replaceAllUsesWith(folded);
                inst->getParent()->getInstructions().remove(inst);
                return true;
            }
        }
    }
    else if (auto cmp = dyn_cast<ICmpInst>(inst)) {
        auto c1 = dyn_cast<Constant>(cmp->getOperand(0));
        auto c2 = dyn_cast<Constant>(cmp->getOperand(1));

        if (c1 && c2) {
            if (auto folded = computeICmp(cmp->getPredicate(), c1, c2)) {
                inst->replaceAllUsesWith(folded);
                inst->getParent()->getInstructions().remove(inst);
                return true;
            }
        }
    }
    else if (auto fcmp = dyn_cast<FCmpInst>(inst)) {
        auto c1 = dyn_cast<Constant>(fcmp->getOperand(0));
        auto c2 = dyn_cast<Constant>(fcmp->getOperand(1));

        if (c1 && c2) {
            if (auto folded = computeFCmp(fcmp->getPredicate(), c1, c2)) {
                inst->replaceAllUsesWith(folded);
                inst->getParent()->getInstructions().remove(inst);
                return true;
            }
        }
    }
    else if (auto castInst = dyn_cast<CastInst>(inst)) {
        if (auto c = dyn_cast<Constant>(castInst->getOperand(0))) {
            if (castInst->getOpID() == Instruction::SIToFP && isa<ConstantInt>(c)) {
                float fval = (float)cast<ConstantInt>(c)->getValue();
                castInst->replaceAllUsesWith(new ConstantFloat(fval));
                castInst->getParent()->getInstructions().remove(castInst);
                return true;
            }
            else if (castInst->getOpID() == Instruction::FPToSI && isa<ConstantFloat>(c)) {
                int ival = (int)cast<ConstantFloat>(c)->getValue();
                castInst->replaceAllUsesWith(new ConstantInt(ival));
                castInst->getParent()->getInstructions().remove(castInst);
                return true;
            }
        }
    }
    // if (true) {
    //     block1;
    // } else {
    //     block2;
    // }
    //
    // become:
    //
    // block1;
    else if (auto br = dyn_cast<BranchInst>(inst)) {
        if (br->getNumOperands() == 3) {
            if (auto cond = dyn_cast<ConstantInt>(br->getOperand(0))) {
                BasicBlock* parent = br->getParent();
                BasicBlock* dest = (cond->getValue() != 0) ? cast<BasicBlock>(br->getOperand(1)) : cast<BasicBlock>(br->getOperand(2));
                parent->getInstructions().remove(br);
                new BranchInst(dest, parent);
                return true;
            }
        }
    }

    return false;
}   

Constant* ConstantFold::computeBinary(Instruction::OpID op, Constant* lhs, Constant* rhs) {
    if (auto i1 = dyn_cast<ConstantInt>(lhs)) {
        if (auto i2 = dyn_cast<ConstantInt>(rhs)) {
            int v1 = i1->getValue();
            int v2 = i2->getValue();
            switch (op) {
                case Instruction::Add: return new ConstantInt(v1 + v2);
                case Instruction::Sub: return new ConstantInt(v1 - v2);
                case Instruction::Mul: return new ConstantInt(v1 * v2);
                case Instruction::Div: return (v2 != 0) ? new ConstantInt(v1 / v2) : nullptr;
                case Instruction::Mod: return (v2 != 0) ? new ConstantInt(v1 % v2) : nullptr;
                default: return nullptr;
            }
        }
    }
    else if (auto f1 = dyn_cast<ConstantFloat>(lhs)) {
        if (auto f2 = dyn_cast<ConstantFloat>(rhs)) {
            float v1 = f1->getValue();
            float v2 = f2->getValue();
            switch (op) {
                case Instruction::Add: return new ConstantFloat(v1 + v2);
                case Instruction::Sub: return new ConstantFloat(v1 - v2);
                case Instruction::Mul: return new ConstantFloat(v1 * v2);
                // Floating point divided by 0 is usually inf and does not crash.
                case Instruction::Div: return new ConstantFloat(v1 / v2);
                // Usually not directly supported or requiring fmod, temporarily ignore here.
                default: return nullptr;
            }
        }
    }
    // TODO
    return nullptr;
}

Constant* ConstantFold::computeICmp(ICmpInst::CmpOp pred, Constant* lhs, Constant* rhs) {
    if (auto i1 = dyn_cast<ConstantInt>(lhs)) {
        if (auto i2 = dyn_cast<ConstantInt>(rhs)) {
            int v1 = i1->getValue();
            int v2 = i2->getValue();
            bool res = false;
            switch (pred) {
                case ICmpInst::EQ: res = (v1 == v2); break;
                case ICmpInst::NE: res = (v1 != v2); break;
                case ICmpInst::SGT: res = (v1 > v2); break;
                case ICmpInst::SGE: res = (v1 >= v2); break;
                case ICmpInst::SLT: res = (v1 < v2); break;
                case ICmpInst::SLE: res = (v1 <= v2); break;
            }
            return new ConstantInt(res ? 1 : 0);
        }
    }
    return nullptr;
}

Constant* ConstantFold::computeFCmp(FCmpInst::CmpOp pred, Constant* lhs, Constant* rhs) {
    if (auto f1 = dyn_cast<ConstantFloat>(lhs)) {
        if (auto f2 = dyn_cast<ConstantFloat>(rhs)) {
            float v1 = f1->getValue();
            float v2 = f2->getValue();
            bool res = false;
            switch (pred) {
                case FCmpInst::OEQ: res = (v1 == v2); break;
                case FCmpInst::ONE: res = (v1 != v2); break;
                case FCmpInst::OGT: res = (v1 > v2); break;
                case FCmpInst::OGE: res = (v1 >= v2); break;
                case FCmpInst::OLT: res = (v1 < v2); break;
                case FCmpInst::OLE: res = (v1 <= v2); break;
            }
            return new ConstantInt(res ? 1 : 0);
        }
    }
    return nullptr;
}