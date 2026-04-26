#include "Optimize/Scalar/DCE.h"
#include <vector>
#include <set>

using namespace sysy;

bool DCE::run() {
    bool anyChanged = false;
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;

        bool changed;
        do {
            changed = eliminateDeadCode(func);
            anyChanged |= changed;
        } while (changed);
    }
    return anyChanged;
}

bool DCE::isInstTrivallyDead(Instruction *inst) {
    // Having a ParentBB is alive Inst.
    for (auto user : inst->getUsers()) {
        if (auto uInst = dyn_cast<Instruction>(user)) {
            // If user is inst itself or user's parent is inst's parent, it is alive.
            // %p1 = phi [ %p1 ]
            if (uInst != inst && uInst->getParent() != nullptr) {
                return false;
            }
        } else {
            return false;
        }
    }

    switch (inst->getOpID()) {
        case Instruction::Ret:
        case Instruction::Br:
        case Instruction::Store:
        case Instruction::Call:
            return false;
        default:
            break;
    }
    return true;
}

bool DCE::eliminateDeadCode(Function *func) {
    bool changed = false;
    std::vector<Instruction*> workList;
    std::set<Instruction*> visited;

    for (auto bb : func->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (isInstTrivallyDead(inst)) {
                workList.push_back(inst);
                visited.insert(inst);
            }
        }
    }

    while (!workList.empty()) {
        Instruction* inst = workList.back();
        workList.pop_back();
        visited.erase(inst);

        if (!isInstTrivallyDead(inst)) continue;

        for (int i = 0; i < inst->getNumOperands(); ++i) {
            Value* op = inst->getOperand(i);
            inst->setOperand(i, nullptr);

            if (auto opInst = dyn_cast<Instruction>(op)) {
                if (opInst != inst && isInstTrivallyDead(opInst) && visited.find(opInst) == visited.end()) {
                    workList.push_back(opInst);
                    visited.insert(opInst);
                }
            }
        }

        inst->replaceAllUsesWith(nullptr);

        inst->eraseInst();
        changed = true;
    }

    return changed;
}
