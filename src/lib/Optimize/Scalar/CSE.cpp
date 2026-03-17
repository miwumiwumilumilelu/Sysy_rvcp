#include "Optimize/Scalar/CSE.h"
#include "Optimize/Analysis/PureFunc.h"

using namespace sysy;

bool CSE::run() {
    bool anyChanged = false;
    bool changed = true;
    purityCache.clear();
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

// fix:
// operandKey is encoded by value instead of pointer to resolve Performance bug caused by the same value.
static uint64_t operandKey(Value* v) {
    if (!v) return 0;
    if (auto* ci = dyn_cast<ConstantInt>(v))
        // int64_t is used to get the value.
        // uint32_t is used to truncate the value to 32 bits.
        // uint64_t is used to zero-extend the value to 64 bits.
        return (1ULL << 63) | (uint64_t)(uint32_t)(int64_t)ci->getValue();
    if (isa<ConstantZero>(v))
        return (1ULL << 63);  // same as ConstantInt(0)
    if (auto* cf = dyn_cast<ConstantFloat>(v)) {
        uint32_t bits = 0;
        float f = cf->getValue();
        __builtin_memcpy(&bits, &f, 4);
        // bit62 is used to distinguish ConstantFloat from ConstantInt.
        // 3ULL <-> 11
        return (3ULL << 62) | bits;
    }
    return (uint64_t)(uintptr_t)v;
}

// Eliminate duplicate pure computations within the bb.
bool CSE::localCSE(BasicBlock* bb) {
    bool changed = false;
    // Key: (opID+subOp, operand0_key, operand1_key)
    using CSEKey = std::tuple<int, uint64_t, uint64_t>;
    // Record the first seen inst with the same key.
    std::map<CSEKey, Instruction*> seen;

    // extend:
    // Pure call CSE: key is [func_ptr, arg0_key, arg1_key, ...]
    std::map<std::vector<uint64_t>, Instruction*> callSeen;
    // load CSE: Find load not yet killed by store.
    std::map<uint64_t, Instruction*> loadSeen;

    std::vector<Instruction*> toRemove;

    for (auto inst : bb->getInstructions()) {
        if (isa<StoreInst>(inst)) {
            loadSeen.clear();
            continue;
        }

        // Same address, no intervening store, then replace with the same value.
        if (isa<LoadInst>(inst)) {
            uint64_t addrKey = operandKey(inst->getOperand(0));
            auto it = loadSeen.find(addrKey);
            if (it != loadSeen.end()) {
                inst->replaceAllUsesWith(it->second);
                toRemove.push_back(inst);
                changed = true;
            } else {
                loadSeen[addrKey] = inst;
            }
            continue;
        }

        // same callee + same args -> same result.
        if (auto* call = dyn_cast<CallInst>(inst)) {
            if (!call->getType()->isVoid() && isPureFunc(call->getFunction(), purityCache)) {
                std::vector<uint64_t> callKey;
                callKey.push_back((uint64_t)(uintptr_t)call->getFunction());
                for (int i = 1; i < (int)call->getNumOperands(); i++)
                    callKey.push_back(operandKey(call->getOperand(i)));

                auto it = callSeen.find(callKey);
                if (it != callSeen.end()) {
                    inst->replaceAllUsesWith(it->second);
                    toRemove.push_back(inst);
                    changed = true;
                } else {
                    callSeen[callKey] = inst;
                }
            }
            continue;
        }

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
        CSEKey key{(int)inst->getOpID() * 4399 + subOp, operandKey(lhs), operandKey(rhs)};

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
