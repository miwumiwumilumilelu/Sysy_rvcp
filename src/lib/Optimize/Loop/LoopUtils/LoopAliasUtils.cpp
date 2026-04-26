#include "Optimize/Loop/LoopUtils/LoopAliasUtils.h"
#include "IR/Instruction.h"
#include <set>

using namespace sysy;

Value* sysy::getLoopBaseObject(Value* v, std::set<Value*>& vis) {
    if (!vis.insert(v).second) return v;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return getLoopBaseObject(gep->getOperand(0), vis);
    if (auto* phi = dyn_cast<PhiInst>(v)) {
        Value* base = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            Value* b = getLoopBaseObject(phi->getOperand(k), vis);
            // Skip cycle-markers (b == v): address recurrence phi created by LoopSR.
            // Its non-self-referential incomings reveal the true base.
            if (b == v) continue;
            if (!base) base = b;
            else if (base != b) return v;
        }
        return base ? base : v;
    }
    return v;
}

Value* sysy::getLoopBaseObject(Value* v) {
    std::set<Value*> vis;
    return getLoopBaseObject(v, vis);
}

void sysy::collectAllBases(Value* v, std::set<Value*>& vis, std::set<Value*>& bases) {
    if (!vis.insert(v).second) return; // cycle guard: already visited, skip
    if (auto* gep = dyn_cast<GetElementPtrInst>(v)) {
        collectAllBases(gep->getOperand(0), vis, bases);
        return;
    }
    if (auto* phi = dyn_cast<PhiInst>(v)) {
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2)
            collectAllBases(phi->getOperand(k), vis, bases);
        return;
    }
    bases.insert(v); // leaf: alloca, global, arg, etc.
}
