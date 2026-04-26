#include "Optimize/Scalar/DSE.h"

using namespace sysy;

bool DSE::run() {
    bool anyChanged = false;
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;
        anyChanged |= runOnFunction(func);
    }
    return anyChanged;
}

std::set<Value*> DSE::computeAliasSet(AllocaInst* alloca) {
    std::set<Value*> aliasSet;
    std::vector<Value*> worklist;

    aliasSet.insert(alloca);
    worklist.push_back(alloca);

    while (!worklist.empty()) {
        Value* val = worklist.back();
        worklist.pop_back();

        for (auto user : val->getUsers()) {
            // GEP whose base pointer operand is val
            if (auto* gep = dyn_cast<GetElementPtrInst>(user)) {
                if (gep->getOperand(0) == val) {
                    if (aliasSet.insert(gep).second)
                        worklist.push_back(gep);
                }
            }
            // Pointer phi: val appears as one of the incoming values
            else if (dyn_cast<PhiInst>(user)) {
                if (aliasSet.insert(user).second)
                    worklist.push_back(user);
            }
        }
    }
    return aliasSet;
}

bool DSE::isDeadAliasSet(const std::set<Value*>& aliasSet) {
    for (auto* val : aliasSet) {
        for (auto user : val->getUsers()) {
            if (dyn_cast<LoadInst>(user)) {
                return false; // live read
            } else if (dyn_cast<CallInst>(user)) {
                return false; // address passed to call -> escaping
            } else if (auto* store = dyn_cast<StoreInst>(user)) {
                // store val, ptr  -> val is the VALUE being stored -> escaping
                if (store->getOperand(0) == val)
                    return false;
                // store val, ptr  where val == ptr operand: normal write, ok
            } else if (auto* gep = dyn_cast<GetElementPtrInst>(user)) {
                // GEP of val that is not in aliasSet means a derived pointer escaped
                if (gep->getOperand(0) == val && aliasSet.find(gep) == aliasSet.end())
                    return false;
            } else if (dyn_cast<PhiInst>(user)) {
                if (aliasSet.find(user) == aliasSet.end())
                    return false;
            } else {
                return false; // unknown user type -> conservative
            }
        }

        if (auto* phi = dyn_cast<PhiInst>(val)) {
            for (int i = 0; i < phi->getNumOperands(); i += 2) {
                if (aliasSet.find(phi->getOperand(i)) == aliasSet.end())
                    return false;
            }
        }
    }
    return true;
}

void DSE::collectDeadStores(const std::set<Value*>& aliasSet,
                             std::set<Instruction*>& dead) {
    for (auto* val : aliasSet) {
        for (auto user : val->getUsers()) {
            if (auto* store = dyn_cast<StoreInst>(user)) {
                if (store->getOperand(1) == val) // val is the ptr operand
                    dead.insert(store);
            }
        }
    }
}

bool DSE::runOnFunction(Function* func) {
    std::set<Instruction*> deadStores;

    for (auto bb : func->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto* alloca = dyn_cast<AllocaInst>(inst)) {
                auto aliasSet = computeAliasSet(alloca);
                if (isDeadAliasSet(aliasSet))
                    collectDeadStores(aliasSet, deadStores);
            }
        }
    }

    if (deadStores.empty())
        return false;

    for (auto* store : deadStores) {
        store->eraseInst();
    }

    return true;
}
