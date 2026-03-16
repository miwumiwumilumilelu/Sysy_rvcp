#include "Optimize/Analysis/LoopInfo.h"
#include "IR/Instruction.h"
#include <algorithm>

using namespace sysy;

LoopInfo::LoopInfo(Function* f, Dominators& dt) : F(f), DT(dt) {
    build(); 
}

void LoopInfo::build() {
    // Find back edges (B->H where H dom B).
    for (auto bb : F->getBody()->getBlocks()) {
        for (auto succ : DT.getSuccessors(bb)) {
            if (!DT.dominates(succ, bb)) continue;

            BasicBlock* head = succ;
            BasicBlock* latch = bb;

            // Reuse existing loop with same header if any.
            Loop* L = nullptr;
            for (auto l : All) 
                if (l->head == head) { L = l; break; }
            if (!L) { L = new Loop(); L->head = head; All.push_back(L); }
            L->latch = latch;

            // Reverse BFS from latch; merge with existing blocks (multiple latches).
            std::set<BasicBlock*> seen(L->blocks.begin(), L->blocks.end());
            seen.insert(head);
            std::vector<BasicBlock*> work;
            if (seen.insert(latch).second) work.push_back(latch);
            while (!work.empty()) {
                auto cur = work.back();
                work.pop_back();
                for (auto pred : DT.getPredecessors(cur)) {
                    if (seen.insert(pred).second) work.push_back(pred);
                }
            }
            L->blocks.clear();
            L->blocks.push_back(head);
            for (auto b : seen) if (b != head) L->blocks.push_back(b);
        }
    }

    // Build loop tree (parent/sub). Prefer tightest enclosing loop.
    for (auto inner : All) {
        for (auto outer : All) {
            if (inner == outer) continue;
            if (!outer->has(inner->head)) continue;
            // More Closely nested loop is preferred.
            if (!inner->up || outer->blocks.size() < inner->up->blocks.size())
                inner->up = outer;
        }
    }

    for (auto L : All) {
        if (!L->up) Tops.push_back(L);
        else L->up->sub.push_back(L);
    }

    for (auto L : All)
        for (auto bb : L->blocks) {
            auto it = BMap.find(bb);
            // More Closely nested loop is preferred.
            if (it == BMap.end() || L->blocks.size() < it->second->blocks.size())
                BMap[bb] = L;
        }

    // Ensure unique preheader for every loop.
    for (auto L : All) ensurePre(L);
}

void LoopInfo::ensurePre(Loop* L) {
    std::vector<BasicBlock*> ext;
    for (auto pred : DT.getPredecessors(L->head)) {
        // Check if pred is in the loop.
        if (!L->has(pred)) ext.push_back(pred);
    }

    if (ext.size() == 1) {
        auto& succs = DT.getSuccessors(ext[0]);
        if (succs.size() == 1 && succs[0] == L->head) { L->pre = ext[0]; return; }
    }

    // Otherwise, insert a new preheader block before L->head.
    auto* region = F->getBody();
    auto* pre = new BasicBlock("pre_" + L->head->getName(), region);
    auto& blist = region->getBlocks();
    auto it = std::find(blist.begin(), blist.end(), L->head);
    blist.splice(it, blist, std::prev(blist.end()));

    new BranchInst(L->head, pre);

    // Redirect all external preds: head → pre.
    for (auto ep : ext) {
        auto term = ep->getInstructions().back();
        if (auto br = dyn_cast<BranchInst>(term)) br->replaceSuccessor(L->head, pre);
    }

    // Fix PhiInst incoming blocks in head: ext -> pre.
    for (auto inst : L->head->getInstructions()) {
        auto phi = dyn_cast<PhiInst>(inst);
        if (!phi) continue;
        for (int i = 1; i < phi->getNumOperands(); i += 2) {
            auto* inBB = static_cast<BasicBlock*>(phi->getOperand(i));
            if (std::find(ext.begin(), ext.end(), inBB) != ext.end())
                phi->setOperand(i, pre);
        }
    }

    BMap[pre] = L->up;
    L->pre = pre;
}
