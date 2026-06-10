#include "../../../include/Optimize/Scalar/LoopUnroll.h"
#include "../../../include/Optimize/Scalar/IRClone.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/LoopInfo.h"
#include "../../../include/Optimize/Analysis/SCEV.h"
#include "../../../include/Optimize/Analysis/ValueTracking.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "../../../include/IR/Instruction.h"
#include <algorithm>
#include <cassert>
#include <queue>
#include <unordered_map>
#include <vector>

using namespace sysy;

static constexpr int MaxMemLoopUnrollTrip = 8;

static bool isSafe(Instruction* inst) {
    switch (inst->getOpID()) {
    case Instruction::Add: case Instruction::Sub:
    case Instruction::Mul: case Instruction::Div: case Instruction::Mod:
    case Instruction::Shl: case Instruction::Ashr:
    case Instruction::And: case Instruction::Or: case Instruction::Xor:
    case Instruction::FAdd: case Instruction::FSub:
    case Instruction::FMul: case Instruction::FDiv:
    case Instruction::ICmp: case Instruction::FCmp:
    case Instruction::SIToFP: case Instruction::FPToSI:
    case Instruction::Load: case Instruction::Store:
    case Instruction::GetElementPtr:
    case Instruction::Select:
        return true;
    default:
        return false;
    }
}

// Check if preheader is the only predecessor.
static bool preEnterSafe(BasicBlock* pre, BasicBlock* head, ValueTracking& vt) {
    if (!pre || pre->getInstructions().empty()) return false;
    auto* br = dyn_cast<BranchInst>(pre->getInstructions().back());
    if (!br) return false;

    if (br->getNumOperands() != 3)
        return false;
    if (br->getNumOperands() == 1)
        return dyn_cast<BasicBlock>(br->getOperand(0)) == head;

    // br cond, A, B
    // A==head || B==head && cond must knowBool
    bool cond = false;
    if (!vt.knownBool(br->getOperand(0), cond))
        return false;
    auto* taken = dyn_cast<BasicBlock>(br->getOperand(cond ? 1 : 2));
    return taken == head;
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
    std::vector<BasicBlock*> bodyOrder;
    bool multiBlock = false;
};

// Returns all loop blocks except head, in topological (processing) order, latch last.
//
// head:
//   br body1
// body1:
//   ...
//   br body2
// bodyx:
//   ...
//   br latch
// latch:
//   i.next = i + 1
//   br cond, head, exit
//
// turns to:
// 
// [body1, body2, ..., latch]
static std::vector<BasicBlock*> getLoopOrder(Loop* L, BasicBlock* head, BasicBlock* latch) {
    std::vector<BasicBlock*> order;
    // Compute in-degrees within L excluding head.
    std::unordered_map<BasicBlock*, int> inDeg;
    for (auto* bb : L->blocks)
        if (bb != head) inDeg[bb] = 0;

    for (auto* bb : L->blocks) {
        if (bb == head) continue;
        auto& il = bb->getInstructions();
        if (il.empty()) continue;
        auto* term = dyn_cast<BranchInst>(il.back());
        if (!term) continue;

        // Topological order requires the graph to be acyclic, therefore back edges are not counted.
        int startIdx = (term->getNumOperands() == 3) ? 1 : 0;
        for (int i = startIdx; i < term->getNumOperands(); ++i) {
            auto* succ = dyn_cast<BasicBlock>(term->getOperand(i));
            if (succ && succ != head && inDeg.count(succ))
                inDeg[succ]++;
        }
    }

    std::queue<BasicBlock*> worklist;
    for (auto& p : inDeg)
        if (p.second == 0) worklist.push(p.first);

    while (!worklist.empty()) {
        auto* bb = worklist.front(); 
        worklist.pop();
        order.push_back(bb);
        auto& il = bb->getInstructions();
        if (il.empty()) continue;
        auto* term = dyn_cast<BranchInst>(il.back());
        if (!term) continue;
        int startIdx = (term->getNumOperands() == 3) ? 1 : 0;
        for (int i = startIdx; i < term->getNumOperands(); ++i) {
            auto* succ = dyn_cast<BasicBlock>(term->getOperand(i));
            if (succ && succ != head && inDeg.count(succ)) {
                if (--inDeg[succ] == 0) worklist.push(succ);
            }
        }
    }

    // Ensure latch is last.
    auto it = std::find(order.begin(), order.end(), latch);
    if (it != order.end() && it != std::prev(order.end())) {
        order.erase(it);
        order.push_back(latch);
    }

    return order;
}

// Match counted-loop shape.
static bool matchLoop(Loop* L, BasicBlock* pre, SCEV& scev, ValueTracking& vt, UnrollInfo& info, int threshold) {
    if (!pre) return false;

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
        // Top-tested shape: must be exactly 2 blocks (head + latch).
        if (L->blocks.size() != 2) return false;
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
        // Latch-tested shape: head must branch into a loop block (latch directly, or a body block).
        auto* headSucc = dyn_cast<BasicBlock>(hbr->getOperand(0));
        if (!headSucc || !L->has(headSucc)) return false;

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

        // Head may contain phis, branch, and dead/hoisted instructions left by
        // LoopRotate (e.g. the rotated exit-cmp that is unused in this block).
        // We simply allow any instruction that does not introduce a new branch.
        for (auto* inst : headInsts) {
            if (isa<BranchInst>(inst) && inst != hbr) return false; // unexpected extra branch
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
        Value* boundVal = nullptr;
        if (ebi.lhsRec && ebi.lhsRec->loop == L && !ebi.rhsRec)
            boundVal = ebi.rhs;
        else if (ebi.rhsRec && ebi.rhsRec->loop == L && !ebi.lhsRec)
            boundVal = ebi.lhs;
        if (!dyn_cast<ConstantInt>(boundVal))
            return false;
        int64_t exactTrips = -1;
        if (!getConstantTripCountFromInfo(ebi, L, exactTrips)) return false;
        if (exitCmpInLatch) {
            if (!preEnterSafe(pre, head, vt)) return false;
            ++exactTrips;
        }
        tripCount = (int)exactTrips;
    }

    if (tripCount < 0 || tripCount > threshold) return false;

    if (tripCount > MaxMemLoopUnrollTrip) {
        for (auto* bb : L->blocks) {
            for (auto* inst : bb->getInstructions()) {
                if (isa<LoadInst>(inst) || isa<StoreInst>(inst))
                    return false;
            }
        }
    }

    // For latch-tested shape: compute body block order and check sizes.
    std::vector<BasicBlock*> bodyOrder;
    bool isMultiBlock = false;
    if (exitCmpInLatch) {
        bodyOrder = getLoopOrder(L, head, latch);
        // bodyOrder must be non-empty and end with latch.
        if (bodyOrder.empty() || bodyOrder.back() != latch) return false;
        isMultiBlock = (bodyOrder.size() > 1);

        // Limit total cloned instructions: bodyOrder.size() * tripCount <= 200.
        if (isMultiBlock && bodyOrder.size() * (size_t)tripCount > 200) return false;

        // Reject body blocks with unsupported instructions (for all body blocks).
        for (auto* bb : bodyOrder) {
            for (auto* inst : bb->getInstructions()) {
                if (isa<BranchInst>(inst)) continue;
                if (inst == ivIncrement) continue;
                if (inst == exitCmpInLatch) continue;
                if (!isSafe(inst)) return false;
            }
        }
    } else {
        // Top-tested shape: only latch.
        for (auto* inst : latch->getInstructions()) {
            if (isa<BranchInst>(inst)) continue;
            if (inst == ivIncrement) continue;
            if (!isSafe(inst)) return false;
        }
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
    info.bodyOrder = std::move(bodyOrder);
    info.multiBlock = isMultiBlock;
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

    // Delete the original loop block(head + latch).
    // head phi uses latch value
    // latch instruction uses head phi
    // latch branch uses head block
    // So Two-pass to delete: 
    auto& blist = region->getBlocks();
    for (auto* inst : head->getInstructions())
        inst->dropAllOperands();
    for (auto* inst : latch->getInstructions())
        inst->dropAllOperands();

    while (!head->getInstructions().empty())
        head->getInstructions().front()->eraseInst();
    blist.remove(head);
    delete head;

    while (!latch->getInstructions().empty())
        latch->getInstructions().front()->eraseInst();
    blist.remove(latch);
    delete latch;

    return true;
}

// Unroll a latch-tested loop with multi-block body (head -> body blocks -> latch).
// pre:
//   br body1_u0
// [body1_u0 -> body2_u0 -> ...-> latch_u0]
// [body1_u1 -> body2_u1 -> ...-> latch_u1]
// ...
// [body1_uN -> body2_uN -> ...-> latch_uN]
// latch_uN -> exit
static bool unrollMultiBlockLoop(Loop* /*L*/, BasicBlock* pre, UnrollInfo& info, Region* region) {
    const int N = info.tripCount;
    BasicBlock* head = info.head;
    BasicBlock* latch = info.latch;
    BasicBlock* exit = info.exit;
    auto& bodyOrder = info.bodyOrder;

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

    // Seed carried values from preheader.
    ValueMap curVal;
    for (auto* phi : info.carriedPhis)
        curVal[phi] = getPreIncoming(phi);
    curVal[info.ivPhi] = new ConstantInt(info.start);

    // Remove the old preheader branch.
    auto& insts = pre->getInstructions();
    assert(!insts.empty() && isa<BranchInst>(insts.back()));
    insts.back()->eraseInst();

    BasicBlock* prevLatch = nullptr;

    for (int iter = 0; iter < N; ++iter) {
        curVal[info.ivPhi] = new ConstantInt(info.start + iter * info.stride);
        // Build block clone map for this iteration.
        // such as:
        //  bb2_u0
        //  bb3_u0
        //  bb2_u1
        //  bb3_u1
        std::unordered_map<BasicBlock*, BasicBlock*> uBlockMap;
        for (auto* bb : bodyOrder) {
            std::string cloneName = bb->getName() + "_u" + std::to_string(iter);
            auto* cloneBlock = new BasicBlock(cloneName, region);
            uBlockMap[bb] = cloneBlock;
        }
        // Build a BlockMap (std::map) for remapping: body blocks -> clones.
        BlockMap bmap;
        for (auto& p : uBlockMap) bmap[p.first] = p.second;

        // Value map for this iteration (starts from curVal).
        ValueMap iterMap = curVal;
        // Clone all instructions in body blocks (skip latch's back-edge branch).
        for (auto* bb : bodyOrder) {
            auto* cloneBlock = uBlockMap[bb];
            for (auto* inst : bb->getInstructions()) {
                if (bb == latch && isa<BranchInst>(inst)) continue; // First skip latch.

                auto* cloneI = cloneInst(inst, cloneBlock, iterMap, bmap);
                if (!cloneI) {
                    // Delete all cloned blocks.
                    for (auto& p : uBlockMap) {
                        auto* cb = p.second;
                        region->getBlocks().remove(cb);
                        delete cb;
                    }
                    return false;
                }
                iterMap[inst] = cloneI;
            }
        }

        BasicBlock* iterEntry = uBlockMap[bodyOrder[0]];
        if (iter == 0) {
            // pre -> body_u0
            new BranchInst(iterEntry, pre);
        } else {
            new BranchInst(iterEntry, prevLatch);
        }

        // Record the latch cloned in this iteration.
        prevLatch = uBlockMap[latch];

        // Update curVal for next iteration from latch incoming values.
        for (auto* phi : info.carriedPhis) {
            Value* latchIncoming = getLatchIncoming(phi);
            if (latchIncoming) {
                static const BlockMap emptyBM;
                curVal[phi] = remapValue(latchIncoming, iterMap, emptyBM);
            }
        }
        // IV is updated at the top of the next iteration via new ConstantInt.
    }
    // latch_un -> exit
    new BranchInst(exit, prevLatch);

    // Unlike regular Phi, it doesn't need to derive from dependencies on other instructions,
    // because the trip count is known.
    curVal[info.ivPhi] = new ConstantInt(info.start + N * info.stride);

    // pre -> head
    // head/body/latch -> exit
    // exit:
    //      x.out = phi [x.init, pre], [x.next, latch]
    // 
    // turns to:
    //
    // latch_u31 -> exit
    // exit:
    //      x.out = phi [final_x, latch_u31]
    // Fix exit block phis: the predecessor is now prevLatch instead of latch.
    // Also update the values coming from latch if they're in curVal.
    {
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
                // Remap predecessor block to last latch clone.
                ep->setOperand(i + 1, prevLatch);
                // Remap value if it's a latch-defined carried value.
                Value* v = ep->getOperand(i);
                auto it = latchValToPhi.find(v);
                if (it != latchValToPhi.end())
                    ep->setOperand(i, curVal[it->second]);
            }
            // After replacing preheader's branch, pre no longer flows to exit.
            ep->removeIncomingByBlock(pre);
        }
    }

    // Replace remaining uses of header phis with final curVal.
    for (auto* phi : info.carriedPhis)
        phi->replaceAllUsesWith(curVal[phi]);
    info.ivPhi->replaceAllUsesWith(curVal[info.ivPhi]);

    // delete
    auto& blist = region->getBlocks();
    for (auto* inst : head->getInstructions())
        inst->dropAllOperands();
    for (auto* bb : bodyOrder)
        for (auto* inst : bb->getInstructions())
            inst->dropAllOperands();

    while (!head->getInstructions().empty())
        head->getInstructions().front()->eraseInst();
    blist.remove(head);
    delete head;
    for (auto* bb : bodyOrder) {
        while (!bb->getInstructions().empty())
            bb->getInstructions().front()->eraseInst();
        blist.remove(bb);
        delete bb;
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
                ValueTracking vt(f);
                if (!matchLoop(inner, inner->pre, scev, vt, info, Threshold)) continue;
                bool ok = false;
                if (info.multiBlock)
                    ok = unrollMultiBlockLoop(inner, inner->pre, info, f->getBody());
                else
                    ok = unrollLoop(inner, inner->pre, info, f->getBody());
                if (!ok) continue;
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
