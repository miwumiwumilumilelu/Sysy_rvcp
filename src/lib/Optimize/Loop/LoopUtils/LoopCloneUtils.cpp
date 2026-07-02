#include "../../../../include/Optimize/Loop/LoopUtils/LoopCloneUtils.h"
#include <set>

using namespace sysy;

static bool addExitEdge(LoopOneIterClone& out, BasicBlock* orig, BasicBlock* cloned) {
    for (auto& e : out.exitEdges)
        if (e.first == orig && e.second == cloned) return true;
    out.exitEdges.push_back({orig, cloned});
    return true;
}

static BasicBlock* remapLoopTarget(Loop* L, BasicBlock* exitBB, BasicBlock* src,
                                   BasicBlock* dst, LoopOneIterClone& out) {
    if (!dst) return nullptr;
    auto cloneIt = out.blockMap.find(src);
    if (cloneIt == out.blockMap.end()) return nullptr;
    BasicBlock* clonedSrc = cloneIt->second;

    if (L->has(dst)) {
        if (dst == L->head && src == L->latch) {
            addExitEdge(out, src, clonedSrc);
            return exitBB;
        }
        if (dst == L->head) return nullptr; // non-latch continue needs live-out handling
        auto it = out.blockMap.find(dst);
        return it == out.blockMap.end() ? nullptr : it->second;
    }

    if (dst == exitBB) {
        addExitEdge(out, src, clonedSrc);
        return exitBB;
    }
    return nullptr;
}

static bool fillBranch(BranchInst* clone, BranchInst* src, Loop* L,
                       BasicBlock* exitBB, BasicBlock* srcBB,
                       LoopOneIterClone& out) {
    if (src->getNumOperands() == 1) {
        auto* dst = dyn_cast<BasicBlock>(src->getOperand(0));
        auto* mapped = remapLoopTarget(L, exitBB, srcBB, dst, out);
        if (!mapped) return false;
        clone->setOperand(0, mapped);
        return true;
    }

    if (src->getNumOperands() != 3) return false;
    clone->setOperand(0, remapValue(src->getOperand(0), out.valueMap, out.blockMap));
    auto* t = dyn_cast<BasicBlock>(src->getOperand(1));
    auto* f = dyn_cast<BasicBlock>(src->getOperand(2));
    auto* mt = remapLoopTarget(L, exitBB, srcBB, t, out);
    auto* mf = remapLoopTarget(L, exitBB, srcBB, f, out);
    if (!mt || !mf) return false;
    clone->setOperand(1, mt);
    clone->setOperand(2, mf);
    return true;
}

bool sysy::cloneLoopOneIteration(Loop* L, BasicBlock* exitBB, Region* region,
                                 const ValueMap& initialMap,
                                 LoopOneIterClone& out) {
    if (!L || !L->head || !L->latch || !exitBB || !region) return false;

    out = LoopOneIterClone();
    out.valueMap = initialMap;

    for (auto* bb : L->blocks) {
        auto* cloned = new BasicBlock("", region);
        out.blockMap[bb] = cloned;
        if (bb == L->head) out.entry = cloned;
    }
    if (!out.entry) return false;

    std::vector<std::pair<Instruction*, Instruction*>> work;
    for (auto* bb : L->blocks) {
        auto* dstBB = out.blockMap[bb];
        for (auto* inst : bb->getInstructions()) {
            if (bb == L->head && isa<PhiInst>(inst)) continue;
            auto* cloned = cloneSkeleton(inst, dstBB);
            if (!cloned) return false;
            work.push_back({inst, cloned});
            if (!cloned->getType()->isVoid())
                out.valueMap[inst] = cloned;
        }
    }

    for (auto& [src, cloned] : work) {
        if (auto* br = dyn_cast<BranchInst>(src)) {
            if (!fillBranch(cast<BranchInst>(cloned), br, L, exitBB,
                            src->getParent(), out))
                return false;
            continue;
        }

        if (auto* phi = dyn_cast<PhiInst>(src)) {
            for (int i = 0; i < phi->getNumOperands(); i += 2) {
                auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
                if (!fromBB || !L->has(fromBB)) return false;
            }
        }
        fillOperands(cloned, src, out.valueMap, out.blockMap);
    }

    return !out.exitEdges.empty();
}
