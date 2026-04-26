#include "Optimize/Scalar/LoopUnroll.h"
#include "Optimize/Scalar/IRClone.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/LoopInfo.h"
#include "Optimize/Analysis/SCEV.h"
#include "Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <cassert>
#include <unordered_map>
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
    PhiInst* ivPhi;
    Instruction* ivIncrement;
    Instruction* exitCmp = nullptr; // Latch icmp for latch-tested shape.
    int start;
    int stride;
    int tripCount;
    std::vector<PhiInst*> carriedPhis;
};

// Match counted-loop shape.
static bool matchLoop(Loop* L, BasicBlock* pre, SCEV& scev,
                      UnrollInfo& info, int threshold) {
    if (!pre) return false;
    if (L->blocks.size() != 2) return false;

    BasicBlock* head = L->head;
    BasicBlock* latch = L->latch;

    auto& headInsts = head->getInstructions();
    if (headInsts.empty()) return false;
    auto* hbr = dyn_cast<BranchInst>(headInsts.back());
    if (!hbr) return false;

    auto& latchInsts = latch->getInstructions();
    if (latchInsts.empty()) return false;
    auto* lbr = dyn_cast<BranchInst>(latchInsts.back());
    if (!lbr) return false;

    BasicBlock* testBB = nullptr;
    ICmpInst* cmp = nullptr;
    BasicBlock* exitBB = nullptr;
    Instruction* exitCmpInLatch = nullptr;

    if (hbr->getNumOperands() == 3 && lbr->getNumOperands() == 1) {
        // Top-tested shape.
        if (dyn_cast<BasicBlock>(lbr->getOperand(0)) != head) return false;

        cmp = dyn_cast<ICmpInst>(hbr->getOperand(0));
        if (!cmp) return false;

        auto* t1 = dyn_cast<BasicBlock>(hbr->getOperand(1));
        auto* t2 = dyn_cast<BasicBlock>(hbr->getOperand(2));
        if (!t1 || !t2) return false;

        BasicBlock* bodyEntry = L->has(t1) ? t1 : t2;
        exitBB = L->has(t1) ? t2 : t1;
        if (!L->has(bodyEntry) || L->has(exitBB)) return false;
        if (bodyEntry != latch) return false;

        testBB = head;

        // Allow only phis, cmp, and branch.
        bool seenCmp = false, seenBr = false;
        for (auto* inst : headInsts) {
            if (isa<PhiInst>(inst)) continue;
            if (inst == cmp && !seenCmp && !seenBr) { seenCmp = true; continue; }
            if (isa<BranchInst>(inst) && seenCmp && !seenBr) { seenBr = true; continue; }
            return false;
        }
        if (!seenCmp || !seenBr) return false;
    }
    else if (hbr->getNumOperands() == 1 && lbr->getNumOperands() == 3) {
        // Latch-tested shape after rotate.
        if (dyn_cast<BasicBlock>(hbr->getOperand(0)) != latch) return false;

        cmp = dyn_cast<ICmpInst>(lbr->getOperand(0));
        if (!cmp) return false;

        auto* t1 = dyn_cast<BasicBlock>(lbr->getOperand(1));
        auto* t2 = dyn_cast<BasicBlock>(lbr->getOperand(2));
        if (!t1 || !t2) return false;

        if (t1 == head && t2 != head && !L->has(t2)) { exitBB = t2; }
        else if (t2 == head && t1 != head && !L->has(t1)) { exitBB = t1; }
        else return false;

        testBB = latch;
        exitCmpInLatch = cmp;

        // Allow only phis and branch.
        for (auto* inst : headInsts) {
            if (isa<PhiInst>(inst) || isa<BranchInst>(inst)) continue;
            return false;
        }
    }
    else {
        return false;
    }

    // Collect header phis and identify the induction phi.
    std::vector<PhiInst*> allPhis;
    for (auto inst : headInsts) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        allPhis.push_back(phi);
    }
    if (allPhis.empty()) return false;

    // Identify the induction phi via SCEV AddRec.
    PhiInst* ivPhi = nullptr;
    Instruction* ivIncrement = nullptr;
    int ivStart = 0;
    int ivStride = 0;
    for (auto* phi : allPhis) {
        auto* rec = dyn_cast<SEAddRec>(scev.get(phi));
        if (!rec || rec->loop != L || rec->step == 0) continue;
        auto* startC = dyn_cast<SEConst>(rec->start);
        if (!startC) continue;
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

    // Compute trip count.
    int tripCount = -1;
    {
        ExitBranchInfo ebi;
        if (!analyzeExitBranch(L, testBB, scev, ebi)) return false;
        int64_t exactTrips = -1;
        if (!getConstantTripCountFromInfo(ebi, L, exactTrips)) return false;
        tripCount = (int)exactTrips;
    }

    if (tripCount < 0 || tripCount > threshold) return false;

    // Reject latch bodies with unsupported instructions.
    for (auto inst : latch->getInstructions()) {
        if (isa<BranchInst>(inst)) continue;
        if (inst == ivIncrement) continue;
        if (inst == exitCmpInLatch) continue;
        if (!isSupportedInst(inst)) return false;
    }

    // Each latch operand must map to exactly one header phi.
    if (exitCmpInLatch) {
        std::unordered_map<Value*, int> latchOpCount;
        for (auto* phi : allPhis) {
            for (int i = 0; i < phi->getNumOperands(); i += 2) {
                auto* inBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
                if (inBB == latch) { latchOpCount[phi->getOperand(i)]++; break; }
            }
        }
        for (auto& p : latchOpCount) if (p.second > 1) return false;
        for (auto* inst : exitBB->getInstructions()) {
            auto* ep = dyn_cast<PhiInst>(inst);
            if (!ep) break;
            for (int i = 0; i < ep->getNumOperands(); i += 2) {
                auto* bb = dyn_cast<BasicBlock>(ep->getOperand(i + 1));
                if (bb == latch && !latchOpCount.count(ep->getOperand(i)))
                    return false;
            }
        }
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
    info.exitCmp = exitCmpInLatch;
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
        insts.back()->eraseInst();
    }

    // Emit one body copy per iteration.
    for (int iter = 0; iter < N; ++iter) {
        curVal[info.ivPhi] = new ConstantInt(info.start + iter * info.stride);

        ValueMap iterMap = curVal;

        for (auto inst : latch->getInstructions()) {
            if (isa<BranchInst>(inst)) continue;
            if (inst == info.exitCmp) continue;

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

    // Rewrite latch-tested exit phis.
    if (info.exitCmp) {
        std::unordered_map<Value*, PhiInst*> latchValToPhi;
        for (auto* phi : info.carriedPhis)
            if (Value* lv = getLatchIncoming(phi)) latchValToPhi[lv] = phi;
        if (Value* lv = getLatchIncoming(info.ivPhi)) latchValToPhi[lv] = info.ivPhi;

        std::vector<PhiInst*> exitPhis;
        for (auto* inst : exit->getInstructions()) {
            auto* ep = dyn_cast<PhiInst>(inst);
            if (!ep) break;
            exitPhis.push_back(ep);
        }
        for (auto* ep : exitPhis) {
            for (int i = 0; i < (int)ep->getNumOperands(); i += 2) {
                auto* bb = dyn_cast<BasicBlock>(ep->getOperand(i + 1));
                if (bb != latch) continue;
                Value* v = ep->getOperand(i);
                auto it = latchValToPhi.find(v);
                if (it != latchValToPhi.end())
                    ep->setOperand(i, curVal[it->second]);
            }
            ep->removeIncomingByBlock(pre);
        }
    }

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
        while (!head->getInstructions().empty())
            head->getInstructions().front()->eraseInst();
        blist.remove(head);
        delete head;

        while (!latch->getInstructions().empty())
            latch->getInstructions().front()->eraseInst();
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
