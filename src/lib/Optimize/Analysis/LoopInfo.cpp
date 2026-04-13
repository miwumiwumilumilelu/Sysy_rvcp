#include "Optimize/Analysis/LoopInfo.h"
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
        L->latch = (latches.size() == 1) ? latches[0] : nullptr;
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
        L->blockSet = std::unordered_set<BasicBlock*>(seen.begin(), seen.end());
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
        analyzePreh(L);

    for (auto* L : All)
        analyzeExits(L);
}

void LoopInfo::analyzePreh(Loop* L) {
    L->pre = nullptr;

    std::vector<BasicBlock*> ext;
    for (auto* pred : DT.getPredecessors(L->head)) {
        if (!L->has(pred))
            ext.push_back(pred);
    }

    if (ext.size() == 1) {
        auto& succs = DT.getSuccessors(ext[0]);
        if (succs.size() == 1 && succs[0] == L->head)
            L->pre = ext[0];
    }
}

void LoopInfo::analyzeExits(Loop* L) {
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
