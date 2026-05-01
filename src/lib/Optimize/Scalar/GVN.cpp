#include "Optimize/Scalar/GVN.h"
#include "Optimize/Scalar/ExprKey.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/PureFunc.h"
#include "Optimize/Analysis/AliasAnalysis.h"
#include "IR/Instruction.h"
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

using namespace sysy;

// Drop memory facts only when this store may change their value.
//
// %v = load %p -> tab[%p] = %v
// store %v, %q -> if %q may alias %p, %p still contains %v
// store %v, %p -> redundant, because tab[%p] is still %v
static void killChangedAliases(std::map<Value*, Value*>& tab, Value* sptr, 
                            Value* sval, const AliasAnalysis& aa) {
    for (auto it = tab.begin(); it != tab.end(); ) {
        if (aa.mayAlias(it->first, sptr) && it->second != sval)
            it = tab.erase(it);
        else
            ++it;
    }
}

using PtrSet = std::set<Value*>;

static PtrSet blockTransfer(BasicBlock* bb, PtrSet avail,
                            const AliasAnalysis& aa,
                            std::unordered_map<Function*, bool>& purityCache) {
    PtrSet killedHere;
    for (auto inst : bb->getInstructions()) {
        if (auto* st = dyn_cast<StoreInst>(inst)) {
            Value* p = st->getOperand(1);
            for (auto it = avail.begin(); it != avail.end(); )
                it = aa.mayAlias(*it, p) ? avail.erase(it) : ++it;
            killedHere.insert(p);
        } else if (auto* ld = dyn_cast<LoadInst>(inst)) {
            // Only mark const gv loads available.
            if (auto gvld = dyn_cast<GlobalVariable>(ld->getOperand(0))) {
                if (!gvld->isConst()) {
                    continue;
                }
            }
            Value* p = ld->getOperand(0);
            // Only mark available if p has not been stored to in this block.
            bool fresh = true;
            for (auto* k : killedHere)
                if (aa.mayAlias(k, p)) { fresh = false; break; }
            if (fresh)
                avail.insert(p);
        } else if (auto* call = dyn_cast<CallInst>(inst)) {
            if (!isPureFunc(call->getFunction(), purityCache)) {
                avail.clear();
                killedHere.clear();
            }
        }
    }
    return avail;
}

static std::map<BasicBlock*, PtrSet> computeAvailIn(Function* f, Dominators& dt,
                                    const AliasAnalysis& aa,
                                    std::unordered_map<Function*, bool>& purityCache) {
    // Build RPO via post-order DFS.
    std::vector<BasicBlock*> rpo;
    {
        std::set<BasicBlock*> vis;

        std::function<void(BasicBlock*)> dfs = [&](BasicBlock* bb) {
            vis.insert(bb);
            for (auto* s : dt.getSuccessors(bb))
                if (!vis.count(s)) dfs(s);
            rpo.push_back(bb);
        };

        dfs(f->getEntryBlock());
        std::reverse(rpo.begin(), rpo.end());
    }

    std::map<BasicBlock*, PtrSet> availIn, availOut;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto* bb : rpo) {
            const auto& preds = dt.getPredecessors(bb);
            PtrSet newIn;
            bool first = true;
            // Compute newIn by intersecting all preds' availOut.
            for (auto* pred : preds) {
                auto it = availOut.find(pred);
                // If pred is unreachable, skip it.
                if (it == availOut.end()) continue;
                if (first) { newIn = it->second; first = false; }
                else {
                    PtrSet tmp;
                    for (auto* p : newIn)
                        if (it->second.count(p)) tmp.insert(p);
                    newIn = std::move(tmp);
                }
            }
            auto newOut = blockTransfer(bb, newIn, aa, purityCache);
            if (availIn.find(bb) == availIn.end() ||
                availIn[bb] != newIn || availOut[bb] != newOut) {
                availIn[bb] = newIn;
                availOut[bb] = newOut;
                changed = true;
            }
        }
    }
    // Record safe load points for each block.
    return availIn;
}

bool GVN::run() {
    bool any = false;
    purityCache.clear();
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool GVN::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;

    Dominators dt(f); 
    dt.run();

    std::map<BasicBlock*, std::vector<BasicBlock*>> domCh;
    for (auto bb : f->getBody()->getBlocks())
        domCh[bb] = {};
    for (auto bb : f->getBody()->getBlocks())
        if (auto* idom = dt.getIDom(bb)) domCh[idom].push_back(bb);

    AliasAnalysis aa;
    auto loadAvail = computeAvailIn(f, dt, aa, purityCache);

    std::map<ExprKey, Instruction*> exprTab;
    std::map<CallKey, Instruction*> callTab;
    std::map<Value*, Value*> loadTab;
    bool any = false;

    std::function<void(BasicBlock*)> visit = [&](BasicBlock* bb) {
        std::vector<ExprKey> newExpr;
        std::vector<CallKey> newCall;
        std::vector<std::pair<Instruction*, Value*>> toReplace;
        std::vector<Instruction*> toErase;

        auto savedLoadTab = loadTab;
        {
            auto& avail = loadAvail[bb];
            for (auto it = loadTab.begin(); it != loadTab.end(); )
                it = avail.count(it->first) ? ++it : loadTab.erase(it);
        }

        for (auto inst : bb->getInstructions()) {
            auto op = inst->getOpID();
            if (op == Instruction::Phi || op == Instruction::Br ||
                op == Instruction::Ret || op == Instruction::Alloca)
                continue;

            if (op == Instruction::Store) {
                Value* ptr = inst->getOperand(1);
                Value* val = inst->getOperand(0);

                auto it = loadTab.find(ptr);
                if (it != loadTab.end() && it->second == val) {
                    toErase.push_back(inst);
                    continue;
                }
                
                killChangedAliases(loadTab, ptr, val, aa);
                loadTab[ptr] = val;
                continue;
            }

            if (op == Instruction::Load) {
                Value* ptr = inst->getOperand(0);
                auto it = loadTab.find(ptr);
                if (it != loadTab.end()) {
                    toReplace.push_back({inst, it->second});
                } else {
                    loadTab[ptr] = inst;
                }
                continue;
            }

            if (auto* call = dyn_cast<CallInst>(inst)) {
                bool pure = isPureFunc(call->getFunction(), purityCache);
                if (!pure) loadTab.clear(); // impure call may modify memory
                if (call->getType()->isVoid() || !pure) continue;
                CallKey k;
                k.push_back((uint64_t)(uintptr_t)call->getFunction());
                for (int i = 1; i < (int)call->getNumOperands(); i++)
                    k.push_back(vnKey(call->getOperand(i)));
                auto it = callTab.find(k);
                if (it != callTab.end()) {
                    toReplace.push_back({inst, it->second});
                } else {
                    callTab[k] = inst; 
                    newCall.push_back(k);
                }
                continue;
            }

            ExprKey k = makeExprKey(inst);
            if (k == ExprKey{0, 0, 0}) continue;

            auto it = exprTab.find(k);
            if (it != exprTab.end()) {
                toReplace.push_back({inst, it->second});
            } else {
                exprTab[k] = inst; 
                newExpr.push_back(k);
            }
        }

        // Resolve replacement chains: if rep is itself being replaced,
        // follow the chain to the final value to avoid ghost instructions.
        //
        // %x = load %p
        // %y = load %p toReplace.push_back({%y, %x})
        // store %y, %q bug here: loadTab[%q] = %y, but %y should be replaced with %x.
        // %z = load %q
        // return %z
        //
        // Here, %z should be replaced with %x, not %y.
        {
            std::unordered_map<Value*, Value*> repMap;
            for (auto& [old, rep] : toReplace) repMap[old] = rep;
            auto resolve = [&](Value* v) {
                std::set<Value*> seen;
                while (repMap.count(v) && !seen.count(v)) {
                    seen.insert(v);
                    v = repMap[v];
                }
                return v;
            };
            for (auto& [old, rep] : toReplace) {
                rep = resolve(rep);
            }

            std::map<Value*, Value*> remappedLoadTab;
            for (auto& [ptr, val] : loadTab) {
                remappedLoadTab[resolve(ptr)] = resolve(val);
            }
            loadTab = std::move(remappedLoadTab);
        }
        for (auto& [old, rep] : toReplace) {
            old->replaceAllUsesWith(rep);
            old->eraseInst();
            any = true;
        }
        for (auto* inst : toErase) {
            inst->eraseInst();
            any = true;
        }

        for (auto child : domCh[bb]) visit(child);

        // Restore domtree DFS scope.
        for (auto& k : newExpr) exprTab.erase(k);
        for (auto& k : newCall) callTab.erase(k);
        loadTab = savedLoadTab;
    };

    visit(f->getEntryBlock());
    return any;
}
