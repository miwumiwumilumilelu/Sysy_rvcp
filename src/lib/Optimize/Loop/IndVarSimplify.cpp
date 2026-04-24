#include "Optimize/Loop/IndVarSimplify.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "IR/Instruction.h"
#include <functional>

using namespace sysy;

bool IndVarSimplify::runOnLoop(Loop* L, Dominators& /*dt*/, SCEV& scev) {
    return unifyIndVars(L, scev);
}

bool IndVarSimplify::run() {
    bool any = false;
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool IndVarSimplify::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    Dominators dt(f); dt.run();
    LoopInfo li(f, dt);
    SCEV scev(f, li);

    bool changed = false;
    std::function<void(Loop*)> visit = [&](Loop* L) {
        for (auto sub : L->sub) visit(sub);
        changed |= unifyIndVars(L, scev);
        changed |= simplifyShiftRec(L, scev);
    };
    for (auto top : li.tops()) visit(top);
    return changed;
}

bool IndVarSimplify::simplifyShiftRec(Loop* L, SCEV& scev) {
    if (!L->head || !L->latch || L->latches.size() != 1 || L->exits.size() != 1)
        return false;
    BasicBlock* latch  = L->latch;
    BasicBlock* exitBB = L->exits[0];

    // Analysis latch to extend ExitBranchInfo.
    ExitBranchInfo info;
    if (!analyzeExitBranch(L, latch, scev, info)) return false;

    Value* tripCountVal = nullptr;
    if (info.lhsRec && info.lhsRec->loop == L && !info.rhsRec) {
        auto* startC = dyn_cast<SEConst>(info.lhsRec->start);
        // TODO
        if (startC && (startC->val == 0 || startC->val == 1) &&
            info.lhsRec->step == 1 &&
            info.continuePred == ICmpInst::SLT &&
            isLoopInvariantValue(info.rhs, L))
            tripCountVal = info.rhs;
    }
    if (!tripCountVal) return false;

    struct SRTarget {
        PhiInst* phi;
        Value* initVal;
        BinaryInst* vNext;
        int shiftPerIter;
    };
    std::vector<SRTarget> targets;

    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        if (!phi->getType()->isInt()) continue;
        auto* sr = dyn_cast<SEShiftRec>(scev.get(phi));
        if (!sr || sr->loop != L) continue;

        Value* initVal = nullptr;
        BinaryInst* vNext = nullptr;
        for (int i = 0; i + 1 < phi->getNumOperands(); i += 2) {
            auto* bb = cast<BasicBlock>(phi->getOperand(i + 1));
            if (L->has(bb)) vNext = dyn_cast<BinaryInst>(phi->getOperand(i));
            else initVal = phi->getOperand(i);
        }
        if (initVal && vNext) targets.push_back({phi, initVal, vNext, sr->shiftPerIter});
    }
    if (targets.empty()) return false;

    bool changed = false;
    for (auto& t : targets) {
        std::vector<PhiInst*> lcssaPhis;
        for (auto* ei : exitBB->getInstructions()) {
            auto* lp = dyn_cast<PhiInst>(ei);
            if (!lp) break;
            bool usesVNext = false, initOK = true;
            for (int i = 0; i + 1 < lp->getNumOperands(); i += 2) {
                auto* src = dyn_cast<BasicBlock>(lp->getOperand(i + 1));
                if (!src) continue;
                if (L->has(src)) {
                    if (lp->getOperand(i) == static_cast<Value*>(t.vNext)) usesVNext = true;
                } else {
                    if (lp->getOperand(i) != t.initVal) initOK = false;
                }
            }
            if (usesVNext && initOK) lcssaPhis.push_back(lp);
        }
        if (lcssaPhis.empty()) continue;

        auto& exitInsts = exitBB->getInstructions();
        // Insert pos just after any leading phi.
        auto insertPos = exitInsts.begin();
        while (insertPos != exitInsts.end() && isa<PhiInst>(*insertPos)) ++insertPos;

        static int shiftRecID = 0;
        auto emit = [&](BinaryInst* inst) -> BinaryInst* {
            inst->setName("sr" + std::to_string(shiftRecID++));
            inst->setParent(exitBB);
            exitInsts.insert(insertPos, inst);
            return inst;
        };

        // rawShift = tripCount * shiftPerIter
        Value* rawShift;
        if (t.shiftPerIter == 1) {
            rawShift = tripCountVal;
        } else if ((t.shiftPerIter & (t.shiftPerIter - 1)) == 0) {
            int log2k = __builtin_ctz(static_cast<unsigned>(t.shiftPerIter));
            rawShift = emit(new BinaryInst(Instruction::Shl, tripCountVal, new ConstantInt(log2k), nullptr));
        } else {
            rawShift = emit(new BinaryInst(Instruction::Mul, tripCountVal, new ConstantInt(t.shiftPerIter), nullptr));
        }

        auto* d = emit(new BinaryInst(Instruction::Sub, rawShift, new ConstantInt(31), nullptr));
        auto* sign = emit(new BinaryInst(Instruction::Ashr, d, new ConstantInt(31), nullptr));
        auto* masked = emit(new BinaryInst(Instruction::And, d, sign, nullptr));
        auto* posD = emit(new BinaryInst(Instruction::Sub, d, masked, nullptr));
        auto* clamped = emit(new BinaryInst(Instruction::Sub, rawShift, posD, nullptr));
        auto* exitVal = emit(new BinaryInst(Instruction::Ashr, t.initVal, clamped, nullptr));

        for (auto* lp : lcssaPhis) {
            lp->replaceAllUsesWith(exitVal);
            for (int i = 0; i < lp->getNumOperands(); ++i) lp->setOperand(i, nullptr);
            lp->setParent(nullptr);
            exitInsts.remove(lp);
            delete lp;
        }

        for (int i = 0; i + 1 < t.phi->getNumOperands(); i += 2) {
            auto* bb = dyn_cast<BasicBlock>(t.phi->getOperand(i + 1));
            if (bb && L->has(bb)) { t.phi->setOperand(i, nullptr); break; }
        }

        // Disconnect vNext's operands (removes vNext from phi's UseList)
        for (int i = 0; i < t.vNext->getNumOperands(); ++i)
            t.vNext->setOperand(i, nullptr);
        t.vNext->getParent()->getInstructions().remove(t.vNext);
        delete t.vNext;

        // Clear and remove the ShiftRec phi
        for (int i = 0; i < t.phi->getNumOperands(); ++i) t.phi->setOperand(i, nullptr);
        t.phi->setParent(nullptr);
        L->head->getInstructions().remove(t.phi);
        delete t.phi;

        changed = true;
    }
    return changed;
}

bool IndVarSimplify::unifyIndVars(Loop* L, SCEV& scev) {
    if (!L->head) return false;

    std::vector<PhiInst*> phis;
    for (auto inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        phis.push_back(phi);
    }
    if (phis.size() < 2) return false;

    std::vector<SE*> ses;
    ses.reserve(phis.size());
    for (auto* phi : phis)
        ses.push_back(scev.get(phi));

    bool any = false;
    std::vector<bool> dead(phis.size(), false);
    for (size_t i = 0; i < phis.size(); i++) {
        if (dead[i]) continue;
        if (isa<SEUnknown>(ses[i])) continue;
        for (size_t j = i + 1; j < phis.size(); j++) {
            if (dead[j]) continue;
            if (phis[i]->getType() != phis[j]->getType()) continue;
            if (!scev.equal(ses[i], ses[j])) continue;
            phis[j]->replaceAllUsesWith(phis[i]);
            dead[j] = true;
            any = true;
        }
    }

    for (int k = (int)phis.size() - 1; k >= 0; k--) {
        if (!dead[k]) continue;
        L->head->getInstructions().remove(phis[k]);
    }
    return any;
}
