#include "Optimize/Loop/LICM.h"
#include "Optimize/Loop/LoopUtils/LoopAliasUtils.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

using namespace sysy;

static bool isReadOnlyFunc(Function* f, std::unordered_map<Function*, bool>& cache) {
    if (!f) return false;
    auto* body = f->getBody();
    if (!body || body->getBlocks().empty()) return false;
    auto it = cache.find(f);
    if (it != cache.end()) return it->second;
    cache[f] = true;
    for (auto bb : body->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (isa<StoreInst>(inst)) { cache[f] = false; return false; }
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isReadOnlyFunc(call->getFunction(), cache)) {
                    cache[f] = false; return false;
                }
        }
    }
    return true;
}

bool LICM::runOnLoop(Loop* L, Dominators& dt, SCEV& scev) {
    return hoistLoop(L, dt, scev);
}

bool LICM::run() {
    bool any = false;
    purityCache.clear();
    readOnlyCache.clear();
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool LICM::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    Dominators dt(f); dt.run();
    LoopInfo li(f, dt);
    SCEV scev(f, li);

    bool changed = false;
    std::function<void(Loop*)> visit = [&](Loop* L) {
        for (auto sub : L->sub) visit(sub);
        changed |= hoistLoop(L, dt, scev);
    };
    for (auto top : li.tops()) visit(top);
    return changed;
}

// Domtree DFS LICM. hoistable flag: starts true; Load/Branch sets it false,
// preventing stores in the same domtree subtree from being hoisted.
bool LICM::hoistLoop(Loop* L, Dominators& dt, SCEV& scev) {
    auto* preBlock = L->entryBlock(dt);
    if (!preBlock || !L->latch) return false;
    {
        auto* lbr = dyn_cast<BranchInst>(L->latch->getInstructions().back());
        if (!lbr || lbr->getNumOperands() != 3) return false;
    }

    std::vector<StoreInst*> stores;
    bool impure = false, hasMW = false;
    for (auto bb : L->blocks) {
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst)) stores.push_back(st);
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (!isPureFunc(call->getFunction(), purityCache)) {
                    impure = true;
                    if (!isReadOnlyFunc(call->getFunction(), readOnlyCache))
                        hasMW = true;
                }
            }
        }
    }

    std::map<BasicBlock*, std::vector<BasicBlock*>> domCh;
    for (auto bb : L->blocks) domCh[bb] = {};
    for (auto bb : L->blocks) {
        if (bb == L->head) continue;
        auto* idom = dt.getIDom(bb);
        if (idom && domCh.count(idom)) domCh[idom].push_back(bb);
    }

    std::set<Instruction*> inv;
    auto outside = [&](Value* v) -> bool {
        if (!isa<Instruction>(v)) return true;
        auto* i = cast<Instruction>(v);
        return !L->has(i->getParent()) || inv.count(i);
    };

    auto& pre = preBlock->getInstructions();
    auto ins_pt = std::prev(pre.end());
    bool any = false;

    std::function<void(BasicBlock*, bool)> visit = [&](BasicBlock* bb, bool hoistable) {
        std::vector<Instruction*> toHoist;

        // Only hoist from latch-dominating blocks (executes every iteration).
        bool execsEveryIter = dt.dominates(bb, L->latch);

        for (auto inst : bb->getInstructions()) {
            auto op = inst->getOpID();

            if (op == Instruction::Load || op == Instruction::Br)
                hoistable = false;

            if (op == Instruction::Br  || op == Instruction::Ret ||
                op == Instruction::Phi || op == Instruction::Alloca)
                continue;

            if (op == Instruction::Call) {
                auto* call = cast<CallInst>(inst);
                if (!isPureFunc(call->getFunction(), purityCache)) continue;
                bool ok = true;
                for (int i = 1; i < (int)inst->getNumOperands(); i++)
                    if (!outside(inst->getOperand(i))) { ok = false; break; }
                if (ok) { inv.insert(inst); toHoist.push_back(inst); }
                continue;
            }

            if (op == Instruction::Store) {
                if (!hoistable || impure) continue;
                if (!outside(inst->getOperand(0)) || !outside(inst->getOperand(1))) continue;
                inv.insert(inst); toHoist.push_back(inst);
                continue;
            }

            if (op == Instruction::Load) {
                if (!execsEveryIter || hasMW || !outside(inst->getOperand(0))) continue;
                Value* lb = getLoopBaseObject(inst->getOperand(0));
                SE* lse = scev.get(inst->getOperand(0));
                bool alias = false;
                for (auto* st : stores) {
                    if (inv.count(st)) continue;
                    if (getLoopBaseObject(st->getOperand(1)) != lb) continue;
                    if (!scev.distinct(lse, scev.get(st->getOperand(1)))) { alias = true; break; }
                }
                if (!alias) { inv.insert(inst); toHoist.push_back(inst); }
                continue;
            }

            bool ok = true;
            for (int i = 0; i < (int)inst->getNumOperands(); i++)
                if (!outside(inst->getOperand(i))) { ok = false; break; }
            if (ok && execsEveryIter) { inv.insert(inst); toHoist.push_back(inst); }
        }

        for (auto* inst : toHoist) {
            bb->getInstructions().remove(inst);
            inst->setParent(preBlock);
            pre.insert(ins_pt, inst);
        }
        any |= !toHoist.empty();

        for (auto* child : domCh[bb])
            visit(child, hoistable);
    };

    visit(L->head, true);
    return any;
}
