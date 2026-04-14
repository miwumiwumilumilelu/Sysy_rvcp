#include "Optimize/Scalar/LoopUnroll.h"
#include "Optimize/Scalar/IRClone.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/LoopInfo.h"
#include "Optimize/Analysis/SCEV.h"
#include "Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <cassert>
#include <vector>

using namespace sysy;

static bool isSupportedInst(Instruction* inst) {
    switch (inst->getOpID()) {
    case Instruction::Add: case Instruction::Sub:
    case Instruction::Mul: case Instruction::Div: case Instruction::Mod:
    case Instruction::FAdd: case Instruction::FSub:
    case Instruction::FMul: case Instruction::FDiv:
    case Instruction::ICmp: case Instruction::FCmp:
    case Instruction::SIToFP: case Instruction::FPToSI:
    case Instruction::Load: case Instruction::Store:
    case Instruction::GetElementPtr:
        return true;
    default:
        return false;
    }
}

struct UnrollInfo {
    BasicBlock* head;
    BasicBlock* latch;
    BasicBlock* exit;
    PhiInst* ivPhi; // Induction phi.
    Instruction* ivIncrement; // The specific IV increment instruction in the latch.
    int start; // Initial IV value.
    int stride; // IV step per iteration (nonzero).
    int tripCount; // Exact iteration count.
    // Non-IV carried phis.
    std::vector<PhiInst*> carriedPhis;
};

// Match the simple counted-loop shape handled by this pass.
static bool matchLoop(Loop* L, BasicBlock* pre, SCEV& scev,
                      UnrollInfo& info, int threshold) {
    // Require one preheader and exactly two loop blocks.
    if (!pre) return false;
    if (L->blocks.size() != 2) return false;

    BasicBlock* head = L->head;
    BasicBlock* latch = L->latch;

    // The header must end with a conditional branch.
    auto& headInsts = head->getInstructions();
    if (headInsts.empty()) return false;
    auto* br = dyn_cast<BranchInst>(headInsts.back());
    if (!br || br->getNumOperands() != 3) return false;

    // The condition must be a direct icmp.
    auto* cmp = dyn_cast<ICmpInst>(br->getOperand(0));
    if (!cmp) return false;

    // Identify the unique exit edge.
    BasicBlock* bodyEntry = dyn_cast<BasicBlock>(br->getOperand(1));
    BasicBlock* exitBB = dyn_cast<BasicBlock>(br->getOperand(2));
    if (!bodyEntry || !exitBB) return false;

    // Normalize the in-loop and out-of-loop successors.
    bool bodyIsTrue = L->has(bodyEntry);
    if (!bodyIsTrue) {
        if (!L->has(exitBB)) return false; // Neither successor is in the loop.
        std::swap(bodyEntry, exitBB);
    }
    if (L->has(exitBB)) return false; // Both successors stay in the loop.

    // The loop-taking edge must go to the latch.
    if (bodyEntry != latch) return false;

    // The latch must jump back to the header unconditionally.
    {
        auto& li = latch->getInstructions();
        if (li.empty()) return false;
        auto* lbr = dyn_cast<BranchInst>(li.back());
        if (!lbr || lbr->getNumOperands() != 1) return false;
        if (dyn_cast<BasicBlock>(lbr->getOperand(0)) != head) return false;
    }
    // Collect header phis and identify the induction phi.
    std::vector<PhiInst*> allPhis;
    for (auto inst : headInsts) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        allPhis.push_back(phi);
    }
    if (allPhis.empty()) return false;

    // Reject headers with extra computation.
    {
        bool seenCmp = false, seenBr = false;
        for (auto* inst : headInsts) {
            if (isa<PhiInst>(inst)) continue;
            if (inst == cmp && !seenCmp && !seenBr) { seenCmp = true; continue; }
            if (isa<BranchInst>(inst) && seenCmp && !seenBr) { seenBr = true; continue; }
            return false; // Unexpected instruction in the header.
        }
        if (!seenCmp || !seenBr) return false;
    }

    // Identify the induction phi via SCEV: it must be an AddRec on this loop
    // with a constant start and nonzero constant stride.
    PhiInst* ivPhi = nullptr;
    Instruction* ivIncrement = nullptr; // The one specific IV increment instruction.
    int ivStart = 0;
    int ivStride = 0;
    for (auto* phi : allPhis) {
        auto* rec = dyn_cast<SEAddRec>(scev.get(phi));
        if (!rec || rec->loop != L || rec->step == 0) continue;
        auto* startC = dyn_cast<SEConst>(rec->start);
        if (!startC) continue;
        // Recover the specific latch increment instruction for clone-time skip.
        Value* fromLatch = nullptr;
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            auto* inBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
            if (inBB == latch) { fromLatch = phi->getOperand(i); break; }
        }
        auto* incInst = dyn_cast<Instruction>(fromLatch);
        if (!incInst) continue;
        ivPhi = phi;
        ivIncrement = incInst;
        ivStart = (int)startC->val;
        ivStride = (int)rec->step;
        break;
    }
    if (!ivPhi) return false;

    // The trip count must come from a simple constant-bound compare in the
    // header.  One side of the compare must be an AddRec on this loop with a
    // constant start, the other must be a constant.
    int tripCount = -1;
    {
        ExitBranchInfo ebi;
        if (!analyzeExitBranch(L, head, scev, ebi)) return false;
        int64_t exactTrips = -1;
        if (!getConstantTripCountFromInfo(ebi, L, exactTrips)) return false;
        tripCount = (int)exactTrips;
    }

    if (tripCount < 0 || tripCount > threshold) return false;

    // Reject latch bodies with unsupported instructions.
    for (auto inst : latch->getInstructions()) {
        if (isa<BranchInst>(inst)) continue; // Ignore the backedge branch.
        if (inst == ivIncrement) continue;   // Fold only the specific IV step.
        if (!isSupportedInst(inst)) return false; // Reject unsupported instructions.
    }

    // Collect non-IV carried phis.
    std::vector<PhiInst*> carried;
    for (auto* phi : allPhis)
        if (phi != ivPhi) carried.push_back(phi);

    info.head = head;
    info.latch = latch;
    info.exit = exitBB;
    info.ivPhi = ivPhi;
    info.ivIncrement = ivIncrement;
    info.start = ivStart;
    info.stride = ivStride;
    info.tripCount = tripCount;
    info.carriedPhis = std::move(carried);
    return true;
}

// Unroll the matched counted loop.

static bool unrollLoop(Loop* /*L*/, BasicBlock* pre, UnrollInfo& info,
                       Region* region) {
    const int N = info.tripCount;
    BasicBlock* head = info.head;
    BasicBlock* latch = info.latch;
    BasicBlock* exit = info.exit;

    // Track the current value of each carried phi and the IV.
    ValueMap curVal;
    static const BlockMap emptyBlockMap;

    // Seed values from the preheader.
    auto getPreIncoming = [&](PhiInst* phi) -> Value* {
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            auto* inBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
            if (inBB == pre) return phi->getOperand(i);
        }
        return nullptr;
    };
    auto getLatchIncoming = [&](PhiInst* phi) -> Value* {
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            auto* inBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
            if (inBB == latch) return phi->getOperand(i);
        }
        return nullptr;
    };

    for (auto* phi : info.carriedPhis)
        curVal[phi] = getPreIncoming(phi);
    // Seed the IV for direct latch uses.
    curVal[info.ivPhi] = new ConstantInt(info.start);

    // Remove the old preheader branch.
    {
        auto& insts = pre->getInstructions();
        assert(!insts.empty() && isa<BranchInst>(insts.back()));
        delete insts.back();
        insts.pop_back();
    }

    // Emit one body copy per iteration.
    for (int iter = 0; iter < N; ++iter) {
        curVal[info.ivPhi] = new ConstantInt(info.start + iter * info.stride);

        ValueMap iterMap = curVal;

        for (auto inst : latch->getInstructions()) {
            if (isa<BranchInst>(inst)) continue; // Skip the backedge branch.

            if (inst == info.ivIncrement) {
                iterMap[inst] = new ConstantInt(info.start + (iter + 1) * info.stride);
                continue;
            }

            auto* clone = cloneInst(inst, pre, iterMap, emptyBlockMap);
            if (!clone) {
                assert(false && "LoopUnroll: unsupported inst during clone");
                return false;
            }
            iterMap[inst] = clone;
        }

        // Refresh carried values after each copied iteration.
        for (auto* phi : info.carriedPhis) {
            Value* latchIncoming = getLatchIncoming(phi);
            if (latchIncoming)
                curVal[phi] = remapValue(latchIncoming, iterMap, emptyBlockMap);
        }
    }

    // The IV live-out is the value after the final increment.
    curVal[info.ivPhi] = new ConstantInt(info.start + N * info.stride);

    // Jump directly from the preheader to the exit.
    new BranchInst(exit, pre);

    // Rewrite exit uses to the final unrolled values.
    for (auto inst : exit->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) {
            for (int i = 0; i < (int)inst->getNumOperands(); i++) {
                auto* v = inst->getOperand(i);
                auto it = curVal.find(v);
                if (it != curVal.end())
                    inst->setOperand(i, it->second);
            }
            continue;
        }
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            auto* inBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
            if (inBB == head || inBB == latch) {
                Value* oldVal = phi->getOperand(i);
                Value* newVal = remapValue(oldVal, curVal, emptyBlockMap);
                phi->setOperand(i, newVal);
                phi->setOperand(i + 1, pre);
            }
        }
    }

    // Replace remaining uses of header phis with final values.
    for (auto* phi : info.carriedPhis)
        phi->replaceAllUsesWith(curVal[phi]);
    info.ivPhi->replaceAllUsesWith(curVal[info.ivPhi]);

    // Delete the original loop blocks.
    {
        auto& blist = region->getBlocks();
        for (auto* inst : head->getInstructions()) {
            inst->setParent(nullptr);
            delete inst;
        }
        head->getInstructions().clear();
        blist.remove(head);
        delete head;

        for (auto* inst : latch->getInstructions()) {
            inst->setParent(nullptr);
            delete inst;
        }
        latch->getInstructions().clear();
        blist.remove(latch);
        delete latch;
    }

    return true;
}

bool LoopUnroll::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;

    bool anyChanged = false;
    bool changed = true;

    while (changed) {
        changed = false;

        Dominators dt(f);
        dt.run();
        LoopInfo li(f, dt);
        SCEV scev(f, li);

        // Process innermost loops first.
        for (auto* L : li.tops()) {
            std::vector<Loop*> work = {L};
            std::vector<Loop*> innermost;
            while (!work.empty()) {
                auto* cur = work.back(); work.pop_back();
                if (cur->sub.empty()) innermost.push_back(cur);
                else for (auto* s : cur->sub) work.push_back(s);
            }

            for (auto* inner : innermost) {
                UnrollInfo info;
                if (!matchLoop(inner, inner->pre, scev, info, Threshold)) continue;
                if (!unrollLoop(inner, inner->pre, info, f->getBody())) continue;
                changed = anyChanged = true;
                break; // Restart after changing the CFG.
            }
            if (changed) break;
        }
    }

    return anyChanged;
}

bool LoopUnroll::run() {
    bool any = false;
    for (auto f : TheModule->getFunctions())
        any |= runFunc(f);
    return any;
}
