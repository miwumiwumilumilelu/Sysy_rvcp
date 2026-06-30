#include "../../../include/Optimize/Loop/LoopMemPromote.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopAliasUtils.h"
#include "../../../include/Optimize/Analysis/PureFunc.h"
#include "../../../include/IR/Instruction.h"
#include <functional>
#include <map>
#include <set>

using namespace sysy;

static int lmpID = 0;

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
    if (!L->hasPreheaderAndSingleLatch()) return false;
    if (L->exits.empty()) return false;

    // Only promote loops whose in-loop exit edges all leave from the latch.
    for (auto* exitBB : L->exits) {
        for (auto* pred : dt.getPredecessors(exitBB)) {
            if (!L->has(pred)) continue;
            if (pred != L->latch) return false;
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
        if (stBB != L->latch) continue;

        bool loadAfterStore = false;
        bool seenStore = false;
        for (auto* inst : stBB->getInstructions()) {
            if (inst == st) {
                seenStore = true;
                continue;
            }
            if (seenStore && isa<LoadInst>(inst)) {
                loadAfterStore = true;
                break;
            }
        }
        if (loadAfterStore) continue;

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

        std::vector<std::pair<BasicBlock*, std::vector<BasicBlock*>>> exitPreds;
        bool exitsOk = true;
        for (auto* exitBB : L->exits) {
            auto preds = dt.getPredecessors(exitBB);
            if (preds.empty()) { exitsOk = false; break; }
            for (auto* pred : preds) {
                if (pred == preBlock) continue;
                if (!L->has(pred)) { exitsOk = false; break; }
            }
            if (!exitsOk) break;
            exitPreds.push_back({exitBB, std::move(preds)});
        }
        if (!exitsOk) continue;

        auto* preload = new LoadInst(addr, nullptr);
        preload->setName("lmp_pre" + std::to_string(lmpID));
        preload->setParent(preBlock);
        { auto& ins = preBlock->getInstructions(); ins.insert(std::prev(ins.end()), preload); }

        auto* hphi = new PhiInst(ty, nullptr);
        hphi->setName("lmp" + std::to_string(lmpID++));
        hphi->setParent(L->head);
        hphi->addIncoming(preload, preBlock);
        hphi->addIncoming(sval, L->latch);
        L->head->getInstructions().push_front(hphi);

        for (auto* ld : slot.lds) {
            ld->replaceAllUsesWith(hphi);
            ld->eraseInst();
        }
        st->eraseInst();

        for (auto& [exitBB, preds] : exitPreds) {
            std::vector<std::pair<Value*, BasicBlock*>> incomings;
            incomings.reserve(preds.size());
            for (auto* pred : preds) {
                // If the flow goes directly from preBlock to exitBB, 
                // it means the loop body was never executed, 
                // so the values ​​in memory were not updated by the loop.
                // At this point, the exit store should be reset to its initial value:
                // preload = load addr;
                if (pred == preBlock) {
                    incomings.push_back({preload, pred});
                    continue;
                }
                // With the single in-loop store restricted to the latch,
                // the promoted value remains the loop-carried header value on every other in-loop exit edge.
                incomings.push_back({pred == L->latch ? sval : hphi, pred});
            }

            Value* exitVal = nullptr;
            if (incomings.size() == 1) {
                exitVal = incomings[0].first;
            } else {
                auto* ephi = new PhiInst(ty, nullptr);
                ephi->setName("lmp_exit" + std::to_string(lmpID++));
                ephi->setParent(exitBB);
                for (auto& [val, bb] : incomings)
                    ephi->addIncoming(val, bb);
                auto& ins = exitBB->getInstructions();
                auto it = ins.begin();
                while (it != ins.end() && isa<PhiInst>(*it)) ++it;
                ins.insert(it, ephi);
                exitVal = ephi;
            }

            auto* estore = new StoreInst(exitVal, addr, nullptr);
            estore->setParent(exitBB);
            auto& ins = exitBB->getInstructions();
            auto it = ins.begin();
            while (it != ins.end() && isa<PhiInst>(*it)) ++it;
            ins.insert(it, estore);
        }

        any = true;
    }
    return any;
}
