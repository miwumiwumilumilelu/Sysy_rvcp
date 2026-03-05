#include "Optimize/ConstantFold.h"
#include <algorithm>
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
                    if (foldInstruction(inst, bb)) {
                        changed = true;
                    }
                }
            }
        }
    }
}

static ICmpInst::CmpOp invertPredicate(ICmpInst::CmpOp pred) {
    switch (pred) {
        case ICmpInst::EQ:  return ICmpInst::NE;
        case ICmpInst::NE:  return ICmpInst::EQ;
        case ICmpInst::SGT: return ICmpInst::SLE;
        case ICmpInst::SGE: return ICmpInst::SLT;
        case ICmpInst::SLT: return ICmpInst::SGE;
        case ICmpInst::SLE: return ICmpInst::SGT;
    }
    return pred;
}

bool ConstantFold::foldInstruction(Instruction* inst, BasicBlock* currentBB) {
    if (auto bin = dyn_cast<BinaryInst>(inst)) {
        auto c1 = dyn_cast<Constant>(bin->getOperand(0));
        auto c2 = dyn_cast<Constant>(bin->getOperand(1));

        if (c1 && c2) {
            if (auto folded = computeBinary(bin->getOpID(), c1, c2)) {
                inst->replaceAllUsesWith(folded);
                currentBB->getInstructions().remove(inst);
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
                currentBB->getInstructions().remove(inst);
                return true;
            }
        }

        // Pattern: icmp eq/ne (icmp pred X, Y), 0
        //   eq → icmp inv_pred X, Y
        //   ne → replace with inner directly
        auto pred = cmp->getPredicate();
        auto ci2 = dyn_cast<ConstantInt>(cmp->getOperand(1));
        if ((pred == ICmpInst::EQ || pred == ICmpInst::NE) &&
            !c1 && ci2 && ci2->getValue() == 0) {
            if (auto inner = dyn_cast<ICmpInst>(cmp->getOperand(0))) {
                Value* replacement;
                if (pred == ICmpInst::NE) {
                    replacement = inner;
                } else {
                    auto& instList = currentBB->getInstructions();
                    auto pos = std::find(instList.begin(), instList.end(), inst);
                    auto newCmp = new ICmpInst(invertPredicate(inner->getPredicate()),
                                               inner->getOperand(0), inner->getOperand(1),
                                               currentBB);
                    newCmp->setName(inst->getName());
                    instList.splice(pos, instList, std::prev(instList.end()));
                    replacement = newCmp;
                }
                inst->replaceAllUsesWith(replacement);
                for (int i = 0; i < inst->getNumOperands(); ++i)
                    inst->setOperand(i, nullptr);
                currentBB->getInstructions().remove(inst);
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
                currentBB->getInstructions().remove(inst);
                return true;
            }
        }
    }
    else if (auto castInst = dyn_cast<CastInst>(inst)) {
        if (auto c = dyn_cast<Constant>(castInst->getOperand(0))) {
            if (castInst->getOpID() == Instruction::SIToFP && isa<ConstantInt>(c)) {
                float fval = (float)cast<ConstantInt>(c)->getValue();
                castInst->replaceAllUsesWith(new ConstantFloat(fval));
                currentBB->getInstructions().remove(castInst);
                return true;
            }
            else if (castInst->getOpID() == Instruction::FPToSI && isa<ConstantFloat>(c)) {
                int ival = (int)cast<ConstantFloat>(c)->getValue();
                castInst->replaceAllUsesWith(new ConstantInt(ival));
                currentBB->getInstructions().remove(castInst);
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
                BasicBlock* dest_true = dyn_cast<BasicBlock>(br->getOperand(1));
                BasicBlock* dest_false = dyn_cast<BasicBlock>(br->getOperand(2));

                if (!dest_true || !dest_false) return false;
            
                if (dest_true == dest_false) {
                    return false;
                }

                BasicBlock* dest = (cond->getValue() != 0) ? cast<BasicBlock>(br->getOperand(1)) : cast<BasicBlock>(br->getOperand(2));
                currentBB->getInstructions().remove(br);

                br->replaceAllUsesWith(nullptr);
                for(int i = 0; i < br->getNumOperands(); i++) br->setOperand(i, nullptr);
                br->setParent(nullptr);
                delete br;

                new BranchInst(dest, currentBB);

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