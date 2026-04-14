#ifndef LOOPTRIPUTILS_H
#define LOOPTRIPUTILS_H

#include "Optimize/Analysis/LoopInfo.h"
#include "Optimize/Analysis/SCEV.h"

namespace sysy {

// Describes the conditional exit branch of a loop (testBB's terminator),
// whether the branch lives in the latch or the header.
struct ExitBranchInfo {
    ICmpInst* cmp = nullptr;
    bool exitOnTrue = false;
    ICmpInst::CmpOp rawPred = ICmpInst::EQ;
    ICmpInst::CmpOp continuePred = ICmpInst::EQ;
    Value* lhs = nullptr;
    Value* rhs = nullptr;
    SE* lhsSE = nullptr;
    SE* rhsSE = nullptr;
    SEAddRec* lhsRec = nullptr;
    SEAddRec* rhsRec = nullptr;
};

ICmpInst::CmpOp negateTripPred(ICmpInst::CmpOp p);
ICmpInst::CmpOp swapTripPred(ICmpInst::CmpOp p);

bool isLoopInvariantValue(Value* v, Loop* L);

// Populate ExitBranchInfo by inspecting testBB's terminating conditional
// branch (one successor in L, one outside). testBB may be the latch or the
// header, depending on whether the loop is latch-tested or top-tested.
bool analyzeExitBranch(Loop* L, BasicBlock* testBB, SCEV& scev,
                       ExitBranchInfo& out);

bool hasKnownFiniteTripCount(const ExitBranchInfo& info, Loop* L);
bool hasKnownFiniteTripCount(Loop* L, SCEV& scev);

bool getConstantTripCountForCompare(int64_t start, int64_t step, int64_t bound,
                                ICmpInst::CmpOp continuePred,
                                int64_t& tripCount);

// Extract a constant trip count from ExitBranchInfo: one side must be an
// SEAddRec on L with an SEConst start, the other side must be SEConst.
bool getConstantTripCountFromInfo(const ExitBranchInfo& info, Loop* L,
                                  int64_t& tripCount);

}

#endif
