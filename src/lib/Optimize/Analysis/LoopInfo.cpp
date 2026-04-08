#include "Optimize/Analysis/LoopInfo.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace sysy;

LoopInfo::LoopInfo(Function* f, Dominators& dt) : F(f), DT(dt) {
    build();
}

void LoopInfo::build() {
    std::map<BasicBlock*, std::vector<BasicBlock*>> backedges;

    // Group all backedges by header. 
    for (auto* bb : F->getBody()->getBlocks()) {
        for (auto* succ : DT.getSuccessors(bb)) {
            if (!DT.dominates(succ, bb)) continue;
            backedges[succ].push_back(bb);
        }
    }

    // Build one loop per header，
    // and union all blocks reachable backwards from every latch to the header.
    for (auto& [head, latches] : backedges) {
        auto* L = new Loop();
        L->head = head;
        L->latches = latches;
        L->latch = latches.empty() ? nullptr : latches.back();
        All.push_back(L);

        std::set<BasicBlock*> seen;
        std::vector<BasicBlock*> work;
        seen.insert(head);
        for (auto* latch : latches) {
            if (seen.insert(latch).second)
                work.push_back(latch);
        }

        while (!work.empty()) {
            auto* cur = work.back();
            work.pop_back();
            if (cur == head) continue;
            for (auto* pred : DT.getPredecessors(cur)) {
                if (seen.insert(pred).second)
                    work.push_back(pred);
            }
        }

        L->blocks.assign(seen.begin(), seen.end());
        auto it = std::find(L->blocks.begin(), L->blocks.end(), head);
        if (it != L->blocks.end())
            std::iter_swap(L->blocks.begin(), it);
    }

    // Build loop tree. Prefer the smallest enclosing loop as parent.
    for (auto* inner : All) {
        for (auto* outer : All) {
            if (inner == outer) continue;
            if (!outer->has(inner->head)) continue;
            if (!inner->up || outer->blocks.size() < inner->up->blocks.size())
                inner->up = outer;
        }
    }

    for (auto* L : All) {
        if (!L->up) Tops.push_back(L);
        else L->up->sub.push_back(L);
    }

    for (auto* L : All) {
        for (auto* bb : L->blocks) {
            auto it = BMap.find(bb);
            if (it == BMap.end() || L->blocks.size() < it->second->blocks.size())
                BMap[bb] = L;
        }
    }

    for (auto* L : All)
        buildPrehBB(L);

    for (auto* L : All)
        buildExits(L);
}

void LoopInfo::buildPrehBB(Loop* L) {
    std::vector<BasicBlock*> ext;
    for (auto* pred : DT.getPredecessors(L->head)) {
        // Check if pred is back-edge from latchs.
        if (!L->has(pred))
            ext.push_back(pred);
    }

    if (ext.size() == 1) {
        auto& succs = DT.getSuccessors(ext[0]);
        if (succs.size() == 1 && succs[0] == L->head) {
            L->pre = ext[0];
            return;
        }
    }

    auto* region = F->getBody();
    // Add prehBB in the end.
    auto* prehBB = new BasicBlock("pre_" + L->head->getName(), region);
    auto& blist = region->getBlocks();
    auto itHead = std::find(blist.begin(), blist.end(), L->head);
    blist.splice(itHead, blist, std::prev(blist.end()));

    // Rewire all non-backedge predecessors to the new preheader.
    for (auto* ep : ext) {
        auto* term = ep->getInstructions().back();
        if (auto* br = dyn_cast<BranchInst>(term))
            br->replaceSuccessor(L->head, prehBB);
    }

    // Canonicalize header phis so external values first merge in the preheader.
    auto& preInsts = prehBB->getInstructions();
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;

        // Find all incoming from outside the loop, and merge them with a new preheader phi.
        std::vector<std::pair<Value*, BasicBlock*>> forwarded;
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            auto* val = phi->getOperand(i);
            auto* from = cast<BasicBlock>(phi->getOperand(i + 1));
            if (std::find(ext.begin(), ext.end(), from) != ext.end())
                forwarded.push_back({val, from});
        }

        if (forwarded.empty()) continue;

        Value* merged = forwarded[0].first;
        if (forwarded.size() > 1) {
            auto* prePhi = new PhiInst(phi->getType(), nullptr);
            prePhi->setName(phi->getName() + ".ph");
            for (auto& [val, from] : forwarded)
                prePhi->addIncoming(val, from);
            prePhi->setParent(prehBB);
            preInsts.push_back(prePhi);
            merged = prePhi;
        }

        for (auto& [_, from] : forwarded)
            phi->removeIncomingByBlock(from);
        phi->addIncoming(merged, prehBB);
    }

    new BranchInst(L->head, prehBB);
    BMap[prehBB] = L->up;
    L->pre = prehBB;
}

void LoopInfo::buildExits(Loop* L) {
    std::set<BasicBlock*> exiting;
    std::set<BasicBlock*> exits;

    for (auto* bb : L->blocks) {
        auto* term = bb->getInstructions().empty() ? nullptr : bb->getInstructions().back();
        auto* br = dyn_cast<BranchInst>(term);
        if (!br) continue;

        int begin = br->getNumOperands() == 1 ? 0 : 1;
        bool isExiting = false;
        for (int i = begin; i < br->getNumOperands(); ++i) {
            auto* succ = dyn_cast<BasicBlock>(br->getOperand(i));
            if (!succ || L->has(succ)) continue;
            isExiting = true;
            exits.insert(succ);
        }
        if (isExiting)
            exiting.insert(bb);
    }

    L->exiting.assign(exiting.begin(), exiting.end());
    L->exits.assign(exits.begin(), exits.end());
}
