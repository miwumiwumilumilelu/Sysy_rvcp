#include "../../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include <set>

using namespace sysy;

ICmpInst::CmpOp sysy::negateTripPred(ICmpInst::CmpOp p) {
    switch (p) {
        case ICmpInst::SLT: return ICmpInst::SGE;
        case ICmpInst::SLE: return ICmpInst::SGT;
        case ICmpInst::SGT: return ICmpInst::SLE;
        case ICmpInst::SGE: return ICmpInst::SLT;
        case ICmpInst::NE: return ICmpInst::EQ;
        case ICmpInst::EQ: return ICmpInst::NE;
        default: return p;
    }
}

ICmpInst::CmpOp sysy::swapTripPred(ICmpInst::CmpOp p) {
    switch (p) {
        case ICmpInst::SLT: return ICmpInst::SGT;
        case ICmpInst::SLE: return ICmpInst::SGE;
        case ICmpInst::SGT: return ICmpInst::SLT;
        case ICmpInst::SGE: return ICmpInst::SLE;
        default: return p;
    }
}

bool sysy::isLoopInvariantValue(Value* v, Loop* L) {
    if (!L) return false;
    if (auto* i = dyn_cast<Instruction>(v))
        return !L->has(i->getParent());
    return true;
}

bool sysy::analyzeExitBranch(Loop* L, BasicBlock* testBB, SCEV& scev,
                             ExitBranchInfo& out) {
    if (!L || !testBB || testBB->getInstructions().empty()) return false;

    auto* br = dyn_cast<BranchInst>(testBB->getInstructions().back());
    if (!br || br->getNumOperands() != 3) return false;

    auto* cmp = dyn_cast<ICmpInst>(br->getOperand(0));
    if (!cmp) return false;

    auto* trueSucc = dyn_cast<BasicBlock>(br->getOperand(1));
    auto* falseSucc = dyn_cast<BasicBlock>(br->getOperand(2));
    if (!trueSucc || !falseSucc) return false;

    bool trueInLoop = L->has(trueSucc);
    bool falseInLoop = L->has(falseSucc);
    if (trueInLoop == falseInLoop) return false;

    out.cmp = cmp;
    out.exitOnTrue = !trueInLoop;
    out.rawPred = cmp->getPredicate();
    out.continuePred = out.exitOnTrue ? negateTripPred(out.rawPred) : out.rawPred;
    out.lhs = cmp->getOperand(0);
    out.rhs = cmp->getOperand(1);
    out.lhsSE = scev.get(out.lhs);
    out.rhsSE = scev.get(out.rhs);
    out.lhsRec = dyn_cast<SEAddRec>(out.lhsSE);
    out.rhsRec = dyn_cast<SEAddRec>(out.rhsSE);
    return true;
}

bool sysy::getConstantTripCountForCompare(int64_t start, int64_t step,
                                        int64_t bound, ICmpInst::CmpOp continuePred,
                                        int64_t& tripCount) {
    if (step == 0) return false;

    switch (continuePred) {
        case ICmpInst::SLT: {
            if (step <= 0) return false;
            if (start >= bound) { tripCount = 0; return true; }
            tripCount = (bound - start + step - 1) / step;
            return true;
        }
        case ICmpInst::SLE: {
            if (step <= 0) return false;
            if (start > bound) { tripCount = 0; return true; }
            tripCount = (bound - start) / step + 1;
            return true;
        }
        case ICmpInst::SGT: {
            if (step >= 0) return false;
            if (start <= bound) { tripCount = 0; return true; }
            tripCount = (start - bound + (-step) - 1) / (-step);
            return true;
        }
        case ICmpInst::SGE: {
            if (step >= 0) return false;
            if (start < bound) { tripCount = 0; return true; }
            tripCount = (start - bound) / (-step) + 1;
            return true;
        }
        case ICmpInst::NE: {
            int64_t diff = bound - start;
            if (diff == 0) { tripCount = 0; return true; }
            if (step > 0 && diff > 0 && diff % step == 0) {
                tripCount = diff / step;
                return true;
            }
            if (step < 0 && diff < 0 && (-diff) % (-step) == 0) {
                tripCount = (-diff) / (-step);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool sysy::getConstantTripCountFromInfo(const ExitBranchInfo& info, Loop* L,
                                        int64_t& tripCount) {
    if (!L || !info.cmp) return false;

    // Both sides varying on this loop: no single IV to read start/step from.
    if (info.lhsRec && info.lhsRec->loop == L &&
        info.rhsRec && info.rhsRec->loop == L) {
        return false;
    }

    SEAddRec* ivRec = nullptr;
    SEConst* boundCst = nullptr;
    ICmpInst::CmpOp contPred = info.continuePred;
    if (info.lhsRec && info.lhsRec->loop == L && !info.rhsRec) {
        ivRec = info.lhsRec;
        boundCst = dyn_cast<SEConst>(info.rhsSE);
    } else if (info.rhsRec && info.rhsRec->loop == L && !info.lhsRec) {
        ivRec = info.rhsRec;
        boundCst = dyn_cast<SEConst>(info.lhsSE);
        contPred = swapTripPred(info.continuePred);
    }
    if (!ivRec || !boundCst) return false;

    auto* startCst = dyn_cast<SEConst>(ivRec->start);
    if (!startCst) return false;

    return getConstantTripCountForCompare(startCst->val, ivRec->step,
                                          boundCst->val, contPred, tripCount);
}

bool sysy::hasKnownFiniteTripCount(const ExitBranchInfo& info, Loop* L) {
    if (!L || !info.cmp) return false;

    if (info.lhsRec && info.lhsRec->loop == L &&
        info.rhsRec && info.rhsRec->loop == L) {
        return false;
    }

    auto checkMonotone = [&](SEAddRec* rec, ICmpInst::CmpOp pred, Value* bound) {
        if (!rec || rec->loop != L || rec->step == 0) return false;
        if (!isLoopInvariantValue(bound, L)) return false;
        if (rec->step > 0 && (pred == ICmpInst::SLT || pred == ICmpInst::SLE))
            return true;
        if (rec->step < 0 && (pred == ICmpInst::SGT || pred == ICmpInst::SGE))
            return true;
        return false;
    };

    if (checkMonotone(info.lhsRec, info.continuePred, info.rhs)) return true;
    if (checkMonotone(info.rhsRec, swapTripPred(info.continuePred), info.lhs)) return true;

    if (info.continuePred != ICmpInst::NE) return false;

    int64_t trips = 0;
    return getConstantTripCountFromInfo(info, L, trips);
}

bool sysy::hasKnownFiniteTripCount(Loop* L, SCEV& scev) {
    if (!L) return false;
    ExitBranchInfo info;
    return analyzeExitBranch(L, L->latch, scev, info) &&
           hasKnownFiniteTripCount(info, L);
}
