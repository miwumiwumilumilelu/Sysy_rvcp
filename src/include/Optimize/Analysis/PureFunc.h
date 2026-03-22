#ifndef PUREFUNC_H
#define PUREFUNC_H

#include "IR/Module.h"
#include "IR/Instruction.h"
#include <unordered_map>

namespace sysy {

inline bool isPureFunc(Function* f, std::unordered_map<Function*, bool>& cache) {
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;

    auto it = cache.find(f);
    if (it != cache.end()) return it->second;

    // Reject pointer / array parameters
    for (auto* arg : f->getArgs())
        if (arg->getType()->isPointer() || arg->getType()->isArray()) {
            cache[f] = false;
            return false;
        }

    cache[f] = true;

    for (auto bb : f->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {

            // Any store or load (of global/heap memory) makes the function non-pure
            if (isa<StoreInst>(inst) || isa<LoadInst>(inst)) {
                cache[f] = false;
                return false;
            }

            // All Callee are also pure functions (transitive pure)
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (!isPureFunc(call->getFunction(), cache)) {
                    cache[f] = false;
                    return false;
                }
            }
        }
    }
    return true;
}

}
#endif
