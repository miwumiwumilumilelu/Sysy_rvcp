#include "Optimize/CSE.h"

using namespace sysy;

bool CSE::run() {
    bool anyChanged = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto func : TheModule->getFunctions()) {
            for (auto bb : func->getBody()->getBlocks()) {
                if (localCSE(bb)) { changed = true; anyChanged = true; }
            }
        }
    }
    return anyChanged;
}

// Eliminate duplicate pure computations within the bb.
bool CSE::localCSE(BasicBlock* bb) {
    bool changed = false;
    // Key: (opID, lhs, rhs)
    using CSEKey = std::tuple<int, Value*, Value*>;
    // Record the first seen inst with the same key.
    std::map<CSEKey, Instruction*> seen;
    std::vector<Instruction*> toRemove;

    for (auto inst : bb->getInstructions()) {
        // Distinguish the conditions of the CmpInst.
        int subOp = 0;
        bool candidate = false;

        if (isa<BinaryInst>(inst) || isa<CastInst>(inst) || isa<GetElementPtrInst>(inst)) {
            candidate = true;
        } else if (auto cmp = dyn_cast<ICmpInst>(inst)) {
            subOp = (int)cmp->getPredicate();
            candidate = true;
        } else if (auto fcmp = dyn_cast<FCmpInst>(inst)) {
            subOp = (int)fcmp->getPredicate();
            candidate = true;
        }

        if (!candidate) continue;

        Value* lhs = inst->getNumOperands() > 0 ? inst->getOperand(0) : nullptr;
        Value* rhs = inst->getNumOperands() > 1 ? inst->getOperand(1) : nullptr;
        CSEKey key{(int)inst->getOpID() * 4399 + subOp, lhs, rhs};

        auto it = seen.find(key);
        if (it != seen.end()) {
            inst->replaceAllUsesWith(it->second);
            toRemove.push_back(inst);
            changed = true;
        } else {
            seen[key] = inst;
        }
    }

    for (auto inst : toRemove) bb->getInstructions().remove(inst);
    return changed;
}
