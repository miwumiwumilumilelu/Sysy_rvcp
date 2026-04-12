#ifndef LOOPTRIPUTILS_H
#define LOOPTRIPUTILS_H

#include "Optimize/Analysis/LoopInfo.h"
#include "Optimize/Analysis/SCEV.h"

namespace sysy {

struct LatchTripInfo {
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

bool analyzeLatchTripInfo(Loop* L, SCEV& scev, LatchTripInfo& out);
bool hasKnownFiniteTripCount(const LatchTripInfo& info, Loop* L);
bool hasKnownFiniteTripCount(Loop* L, SCEV& scev);

bool getConstantTripCountForCompare(int64_t start, int64_t step, int64_t bound,
                                ICmpInst::CmpOp continuePred,
                                int64_t& tripCount);

}

#endif
