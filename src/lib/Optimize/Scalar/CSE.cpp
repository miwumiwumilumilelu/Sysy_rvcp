#include "Optimize/Scalar/CSE.h"
#include "Optimize/Scalar/ExprKey.h"
#include "Optimize/Analysis/PureFunc.h"

using namespace sysy;

bool CSE::run() {
    bool any = false;
    purityCache.clear();
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto f : TheModule->getFunctions())
            for (auto bb : f->getBody()->getBlocks())
                if (localCSE(bb)) { changed = any = true; }
    }
    return any;
}

bool CSE::localCSE(BasicBlock* bb) {
    bool changed = false;
    std::map<ExprKey, Instruction*> seen;
    std::map<CallKey, Instruction*> callSeen;
    std::map<uint64_t, Instruction*> loadSeen; // addr key -> load; killed by any store
    std::vector<Instruction*> toRemove;

    for (auto inst : bb->getInstructions()) {
        if (isa<StoreInst>(inst)) { loadSeen.clear(); continue; }

        if (isa<LoadInst>(inst)) {
            uint64_t k = vnKey(inst->getOperand(0));
            auto it = loadSeen.find(k);
            if (it != loadSeen.end()) {
                inst->replaceAllUsesWith(it->second);
                toRemove.push_back(inst);
                changed = true;
            } else {
                loadSeen[k] = inst;
            }
            continue;
        }

        if (auto* call = dyn_cast<CallInst>(inst)) {
            if (!call->getType()->isVoid() && isPureFunc(call->getFunction(), purityCache)) {
                CallKey k;
                k.push_back((uint64_t)(uintptr_t)call->getFunction());
                for (int i = 1; i < (int)call->getNumOperands(); i++)
                    k.push_back(vnKey(call->getOperand(i)));
                auto it = callSeen.find(k);
                if (it != callSeen.end()) {
                    inst->replaceAllUsesWith(it->second);
                    toRemove.push_back(inst);
                    changed = true;
                } else {
                    callSeen[k] = inst;
                }
            }
            continue;
        }

        ExprKey k = makeExprKey(inst);
        if (k == ExprKey{0, 0, 0}) continue;

        auto it = seen.find(k);
        if (it != seen.end()) {
            inst->replaceAllUsesWith(it->second);
            toRemove.push_back(inst);
            changed = true;
        } else {
            seen[k] = inst;
        }
    }

    for (auto inst : toRemove) inst->eraseInst();
    return changed;
}
