#include "Optimize/Scalar/DSE.h"
#include "Optimize/Analysis/AliasAnalysis.h"
#include "Optimize/Analysis/Dominators.h"
#include <deque>
#include <map>

using namespace sysy;

// Trace GEP chain back to alloca base,
// conservative (skips phi).
static AllocaInst* baseAllocaOf(Value* ptr) {
    while (auto* gep = dyn_cast<GetElementPtrInst>(ptr))
        ptr = gep->getOperand(0);
    return dyn_cast<AllocaInst>(ptr);
}

static AllocaInst* fixedAllocaOf(Value* ptr) {
    while (auto* gep = dyn_cast<GetElementPtrInst>(ptr)) {
        for (int i = 1; i < gep->getNumOperands(); ++i) {
            if (!isa<ConstantInt>(gep->getOperand(i)))
                return nullptr;
        }
        ptr = gep->getOperand(0);
    }
    return dyn_cast<AllocaInst>(ptr);
}

bool DSE::run() {
    bool anyChanged = false;
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;
        anyChanged |= runOnFunction(func);
    }
    anyChanged |= runUnreadGlobalStores();
    return anyChanged;
}

bool DSE::runOnFunction(Function* func) {
    bool c = runStoreLiveness(func);
    c |= runLocalPeepholes(func);
    return c;
}

std::set<Value*> DSE::computeDerivedPointers(Value* base) {
    std::set<Value*> set;
    std::vector<Value*> worklist{base};
    set.insert(base);
    while (!worklist.empty()) {
        Value* val = worklist.back(); worklist.pop_back();
        for (auto* user : val->getUsers()) {
            if (auto* gep = dyn_cast<GetElementPtrInst>(user)) {
                if (gep->getOperand(0) == val && set.insert(gep).second)
                    worklist.push_back(gep);
            } else if (auto* phi = dyn_cast<PhiInst>(user)) {
                if (set.insert(phi).second)
                    worklist.push_back(phi);
            }
        }
    }
    return set;
}

bool DSE::runStoreLiveness(Function* func) {
    Dominators dt(func);
    dt.run();
    AliasAnalysis aa;

    // Collect escaped allocas: address passed to call/return, or stored as a value.
    // baseAllocaOf skips phi (conservative for deletion, unsound for escape), 
    // so we also scan store-value and return operands to cover the most critical patterns.
    std::set<AllocaInst*> escaped;
    auto markEscaped = [&](Value* v) {
        if (auto* a = baseAllocaOf(v)) escaped.insert(a);
    };
    for (auto* bb : func->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            if (auto* call = dyn_cast<CallInst>(inst)) {
                for (int i = 1; i < call->getNumOperands(); ++i)
                    markEscaped(call->getOperand(i));
            } else if (auto* ret = dyn_cast<ReturnInst>(inst)) {
                if (ret->getNumOperands() > 0) markEscaped(ret->getOperand(0));
            } else if (auto* st = dyn_cast<StoreInst>(inst)) {
                // alloca address stored as a value into memory.
                if (st->getOperand(0)->getType()->isPointer())
                    markEscaped(st->getOperand(0));
            }
        }
    }

    std::map<BasicBlock*, std::set<StoreInst*>> liveOut;
    std::set<StoreInst*> usedStores;
    std::deque<BasicBlock*> worklist;
    std::set<BasicBlock*> queued;

    for (auto* bb : func->getBody()->getBlocks()) {
        worklist.push_back(bb); queued.insert(bb);
    }

    auto mayAlias = [&](Value* a, Value* b) { return aa.mayAlias(a, b); };
    auto mustKill = [&](Value* a, Value* b) {
        return a == b || aa.query(a, b) == AliasAnalysis::Result::MustAlias;
    };

    while (!worklist.empty()) {
        BasicBlock* bb = worklist.front(); worklist.pop_front(); queued.erase(bb);

        std::set<StoreInst*> curLive;
        for (auto* pred : dt.getPredecessors(bb))
            curLive.insert(liveOut[pred].begin(), liveOut[pred].end());

        for (auto* inst : bb->getInstructions()) {
            if (auto* ld = dyn_cast<LoadInst>(inst)) {
                for (auto* store : curLive)
                    if (mayAlias(store->getOperand(1), ld->getOperand(0)))
                        usedStores.insert(store);
                continue;
            }
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                std::vector<StoreInst*> killed;
                for (auto* live : curLive)
                    if (mustKill(live->getOperand(1), st->getOperand(1)))
                        killed.push_back(live);
                for (auto* k : killed) curLive.erase(k);
                curLive.insert(st);
                continue;
            }
            if (auto* call = dyn_cast<CallInst>(inst)) {
                for (int i = 1; i < call->getNumOperands(); ++i) {
                    Value* arg = call->getOperand(i);
                    if (!arg || !arg->getType()->isPointer()) continue;
                    for (auto* store : curLive)
                        if (mayAlias(store->getOperand(1), arg))
                            usedStores.insert(store);
                }
            }
        }

        if (curLive != liveOut[bb]) {
            liveOut[bb] = curLive;
            for (auto* succ : dt.getSuccessors(bb))
                if (!queued.count(succ)) { worklist.push_back(succ); queued.insert(succ); }
        }
    }

    std::vector<StoreInst*> toErase;
    for (auto* bb : func->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            auto* st = dyn_cast<StoreInst>(inst);
            if (!st || usedStores.count(st)) continue;
            auto* alloca = fixedAllocaOf(st->getOperand(1));
            if (!alloca || escaped.count(alloca)) continue;
            if (!alloca->getParent() || alloca->getParent()->getParentFunc() != func) continue;
            toErase.push_back(st);
        }
    }
    for (auto* st : toErase) st->eraseInst();
    return !toErase.empty();
}

bool DSE::runLocalPeepholes(Function* func) {
    std::set<Instruction*> remove;

    for (auto* bb : func->getBody()->getBlocks()) {
        auto& insts = bb->getInstructions();

        // load p; 
        // store (that load), p;
        // ->  drop the store (identity write-back).
        for (auto it = insts.begin(); it != insts.end(); ++it) {
            auto* load = dyn_cast<LoadInst>(*it);
            if (!load || !load->getParent()) continue;
            Value* addr = load->getOperand(0);
            for (auto runner = std::next(it); runner != insts.end(); ++runner) {
                if (isa<CallInst>(*runner) || isa<LoadInst>(*runner)) break;
                auto* st = dyn_cast<StoreInst>(*runner);
                if (!st) continue;
                if (st->getOperand(0) == load && st->getOperand(1) == addr) {
                    remove.insert(st); continue;
                }
                if (st->getOperand(0) == load) continue;
                break;
            }
        }

        // store a, p; <no load/call>; store b, p  →  drop the first store.
        for (auto it = insts.begin(); it != insts.end(); ++it) {
            auto* st = dyn_cast<StoreInst>(*it);
            if (!st || !st->getParent()) continue;
            Value* addr = st->getOperand(1);
            for (auto runner = std::next(it); runner != insts.end(); ++runner) {
                if (isa<LoadInst>(*runner) || isa<CallInst>(*runner)) break;
                auto* next = dyn_cast<StoreInst>(*runner);
                if (!next) continue;
                if (next->getOperand(1) == addr) { remove.insert(st); continue; }
                break;
            }
        }
    }

    for (auto* inst : remove)
        if (inst->getParent()) inst->eraseInst();
    return !remove.empty();
}

bool DSE::collectDeadStoresToUnreadGlobal(GlobalVariable* glob,
                                          std::set<Instruction*>& dead) {
    auto aliasSet = computeDerivedPointers(glob);

    // Collect stores locally,
    // only merge into dead if the whole global is unread.
    std::set<Instruction*> localDead;

    for (auto* val : aliasSet) {
        for (auto* user : val->getUsers()) {
            if (isa<LoadInst>(user) || isa<CallInst>(user) || isa<ReturnInst>(user))
                return false;

            if (auto* store = dyn_cast<StoreInst>(user)) {
                if (store->getOperand(0) == val) return false;
                if (store->getOperand(1) == val) { localDead.insert(store); continue; }
                return false;
            }
            if (auto* gep = dyn_cast<GetElementPtrInst>(user)) {
                if (gep->getOperand(0) == val && aliasSet.count(gep)) continue;
                return false;
            }
            if (auto* phi = dyn_cast<PhiInst>(user)) {
                if (!aliasSet.count(phi)) return false;
                continue;
            }
            return false;
        }

        if (auto* phi = dyn_cast<PhiInst>(val)) {
            for (int i = 0; i < phi->getNumOperands(); i += 2)
                if (!aliasSet.count(phi->getOperand(i))) return false;
        }
    }

    dead.insert(localDead.begin(), localDead.end());
    return true;
}

bool DSE::runUnreadGlobalStores() {
    std::set<Instruction*> dead;
    for (auto* glob : TheModule->getGlobals())
        collectDeadStoresToUnreadGlobal(glob, dead);
    for (auto* inst : dead)
        if (inst->getParent()) inst->eraseInst();
    return !dead.empty();
}
