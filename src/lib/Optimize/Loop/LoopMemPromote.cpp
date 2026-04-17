#include "Optimize/Loop/LoopMemPromote.h"
#include "Optimize/Loop/LoopUtils/LoopAliasUtils.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <functional>
#include <map>
#include <set>

using namespace sysy;

bool LoopMemPromote::runOnLoop(Loop* L, Dominators& dt, SCEV& /*scev*/) {
    return promoteLoop(L, dt);
}

bool LoopMemPromote::run() {
    bool any = false;
    purityCache.clear();
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool LoopMemPromote::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    Dominators dt(f); dt.run();
    LoopInfo li(f, dt);

    bool changed = false;
    std::function<void(Loop*)> visit = [&](Loop* L) {
        for (auto sub : L->sub) visit(sub);
        changed |= promoteLoop(L, dt);
    };
    for (auto top : li.tops()) visit(top);
    return changed;
}

bool LoopMemPromote::promoteLoop(Loop* L, Dominators& dt) {
    auto* preBlock = L->entryBlock(dt);
    if (!preBlock || !L->latch || !L->head) return false;

    auto* pbr = dyn_cast<BranchInst>(preBlock->getInstructions().back());
    if (!pbr || pbr->getNumOperands() != 3) return false;
    auto* lbr = dyn_cast<BranchInst>(L->latch->getInstructions().back());
    if (!lbr || lbr->getNumOperands() != 3) return false;

    std::set<BasicBlock*> lbbs(L->blocks.begin(), L->blocks.end());

    auto exitOf = [&](BranchInst* br) -> BasicBlock* {
        auto* t1 = cast<BasicBlock>(br->getOperand(1));
        auto* t2 = cast<BasicBlock>(br->getOperand(2));
        return lbbs.count(t1) ? t2 : t1;
    };
    BasicBlock* exitBB = exitOf(pbr);
    if (exitBB != exitOf(lbr)) return false;

    // Single-exit check: no loop block may branch outside except to exitBB.
    for (auto bb : L->blocks) {
        auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
        if (!br) continue;
        int start = (br->getNumOperands() == 1) ? 0 : 1;
        for (int k = start; k < (int)br->getNumOperands(); k++) {
            auto* succ = dyn_cast<BasicBlock>(br->getOperand(k));
            if (succ && !lbbs.count(succ) && succ != exitBB) return false;
        }
    }

    for (auto bb : L->blocks)
        for (auto inst : bb->getInstructions())
            if (auto* c = dyn_cast<CallInst>(inst))
                if (!isPureFunc(c->getFunction(), purityCache)) return false;

    auto outside = [&](Value* v) -> bool {
        if (auto* i = dyn_cast<Instruction>(v)) return !L->has(i->getParent());
        return true;
    };

    struct Slot { std::vector<LoadInst*> lds; std::vector<StoreInst*> sts; };
    std::map<Value*, Slot> slots;
    for (auto bb : L->blocks) {
        for (auto inst : bb->getInstructions()) {
            if (auto* ld = dyn_cast<LoadInst>(inst))
                if (outside(ld->getOperand(0)))
                    slots[ld->getOperand(0)].lds.push_back(ld);
            if (auto* st = dyn_cast<StoreInst>(inst))
                if (outside(st->getOperand(1)))
                    slots[st->getOperand(1)].sts.push_back(st);
        }
    }

    bool any = false;
    for (auto& [addr, slot] : slots) {
        if (slot.lds.empty() || slot.sts.size() != 1) continue;

        StoreInst* st = slot.sts[0];
        Value* sval = st->getOperand(0);

        BasicBlock* stBB = st->getParent();
        if (stBB != L->latch && !dt.dominates(stBB, L->latch)) continue;

        Value* base = getLoopBaseObject(addr);
        bool alias = false;
        for (auto bb : L->blocks) {
            for (auto inst : bb->getInstructions()) {
                if (auto* s = dyn_cast<StoreInst>(inst)) {
                    if (s == st) continue;
                    if (getLoopBaseObject(s->getOperand(1)) == base) { alias = true; break; }
                }
                if (auto* l = dyn_cast<LoadInst>(inst)) {
                    if (std::find(slot.lds.begin(), slot.lds.end(), l) != slot.lds.end()) continue;
                    if (getLoopBaseObject(l->getOperand(0)) == base) { alias = true; break; }
                }
            }
            if (alias) break;
        }
        if (alias) continue;

        Type* ty = slot.lds[0]->getType();

        auto* preload = new LoadInst(addr, nullptr);
        preload->setParent(preBlock);
        { auto& ins = preBlock->getInstructions(); ins.insert(std::prev(ins.end()), preload); }

        auto* hphi = new PhiInst(ty, nullptr);
        hphi->setParent(L->head);
        hphi->addIncoming(preload, preBlock);
        hphi->addIncoming(sval, L->latch);
        L->head->getInstructions().push_front(hphi);

        for (auto* ld : slot.lds) {
            ld->replaceAllUsesWith(hphi);
            ld->getParent()->getInstructions().remove(ld);
        }
        st->getParent()->getInstructions().remove(st);

        auto* ephi = new PhiInst(ty, nullptr);
        ephi->setParent(exitBB);
        ephi->addIncoming(preload, preBlock);
        ephi->addIncoming(sval, L->latch);
        {
            auto& ins = exitBB->getInstructions();
            auto it = ins.begin();
            while (it != ins.end() && isa<PhiInst>(*it)) ++it;
            ins.insert(it, ephi);
        }

        auto* estore = new StoreInst(ephi, addr, nullptr);
        estore->setParent(exitBB);
        {
            auto& ins = exitBB->getInstructions();
            auto it = ins.begin();
            while (it != ins.end() && isa<PhiInst>(*it)) ++it;
            ins.insert(it, estore);
        }

        any = true;
    }
    return any;
}
