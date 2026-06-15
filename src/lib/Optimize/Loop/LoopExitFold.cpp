#include "../../../include/Optimize/Loop/LoopExitFold.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/Range.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "../../../include/IR/Instruction.h"
#include <climits>
#include <functional>
#include <vector>

using namespace sysy;


namespace {

// exit = start + trip*step.
struct LinearTarget {
    PhiInst* lcssaPhi;
    SE* arStart;
    int64_t arStep;
};

// An exit LCSSA phi whose loop value is a geometric/shift recurrence phi >>= k:
// exit = init >> min(31, iters * k).  Keyed on the exit phi, like LinearTarget.
struct ShiftTarget {
    PhiInst* lcssaPhi;
    Value* init;
    int64_t shiftPerIter;
};

// exit = (init % M + C * iters) % M.
struct ModAddTarget {
    PhiInst* lcssaPhi;
    Value* init;
    long C;
    long M;
};

}

static bool visitRealUses(Region* region, Instruction* target, Value* repl) {
    if (!region || !target) return false;

    bool changed = false;
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            if (inst != target) {
                for (int i = 0; i < inst->getNumOperands(); ++i) {
                    if (inst->getOperand(i) == target) {
                        if (repl) inst->setOperand(i, repl);
                        changed = true;
                    }
                }
            }

            for (const auto& sub : inst->getRegions())
                changed |= visitRealUses(sub.get(), target, repl);
        }
    }
    return changed;
}

static Region* realUseRoot(Instruction* target) {
    if (!target || !target->getParent() || !target->getParent()->getParent())
        return nullptr;

    if (auto* f = target->getParent()->getParentFunc())
        return f->getBody();
    return target->getParent()->getParent();
}

static bool hasRealUse(Instruction* target) {
    return visitRealUses(realUseRoot(target), target, nullptr);
}

static bool replaceRealUsesWith(Instruction* target, Value* repl) {
    return visitRealUses(realUseRoot(target), target, repl);
}

bool LoopExitFold::run() {
    bool any = false;
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool LoopExitFold::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    Dominators dt(f); dt.run();
    LoopInfo li(f, dt);
    SCEV scev(f, li);
    RangeAnalysis ra(f);

    bool changed = false;
    std::function<void(Loop*)> visit = [&](Loop* L) {
        for (auto sub : L->sub) visit(sub);
        changed |= deduplicateRec(L, scev);
        changed |= foldExitValues(L, scev, ra);
    };
    for (auto top : li.tops()) visit(top);
    return changed;
}

bool LoopExitFold::foldExitValues(Loop* L, SCEV& scev, RangeAnalysis& ra) {
    if (!L->head || !L->latch || L->latches.size() != 1 || L->exits.size() != 1)
        return false;
    BasicBlock* latch = L->latch;
    BasicBlock* exitBB = L->exits[0];

    // Analyze the loop exit condition, extract the loop induction variable and boundary values.
    ExitBranchInfo info;
    if (!analyzeExitBranch(L, latch, scev, info)) return false;
    if (!info.lhsRec || info.lhsRec->loop != L || info.rhsRec) return false;
    if (!isLoopInvariantValue(info.rhs, L)) return false;

    // Determine if the loop is incrementing or decrementing, 
    // and verify the legality of the comparison operator.
    int ivStep = info.lhsRec->step;
    if (ivStep == 0) return false;
    bool isDecreasing = (ivStep < 0);
    int absStep = isDecreasing ? -ivStep : ivStep;

    bool isSGE = (info.continuePred == ICmpInst::SGE);
    bool isSLE = (info.continuePred == ICmpInst::SLE);
    bool validPred = isDecreasing ? (info.continuePred == ICmpInst::SGT || isSGE)
                                : (info.continuePred == ICmpInst::SLT || isSLE);
    if (!validPred) return false;

    auto* ivStartC = dyn_cast<SEConst>(info.lhsRec->start);
    if (!ivStartC) return false;

    // Collect all inductive variables that are affected by linear changes,
    // and have LCSSA Phi received at the exit.
    std::vector<LinearTarget> linearTargets;
    for (auto* inst : exitBB->getInstructions()) {
        auto* lp = dyn_cast<PhiInst>(inst);
        if (!lp) break;
        if (!lp->getType()->isInt()) continue;
        Value* loopVal = nullptr;
        bool consistent = true;
        for (int i = 0; i + 1 < lp->getNumOperands(); i += 2) {
            auto* src = dyn_cast<BasicBlock>(lp->getOperand(i + 1));
            if (!src || !L->has(src)) 
                continue;
            if (!loopVal) 
                loopVal = lp->getOperand(i);
            else if (loopVal != lp->getOperand(i)) { 
                consistent = false; 
                break; 
            }
        }
        if (!consistent || !loopVal) continue;
        auto* ar = dyn_cast<SEAddRec>(scev.get(loopVal));
        if (!ar || ar->loop != L) continue;
        linearTargets.push_back({lp, ar->start, ar->step});
    }

    // Collect geometric/shift recurrences from the exit, keyed on the exit phi
    // (like LinearTarget): the exit phi's loop value must be recPhi >> k (Ashr) or
    // recPhi / 2^k (Div), where recPhi is a header recurrence whose own latch
    // update is exactly that value. This matches what SCEV's SEShiftRec models.
    std::vector<ShiftTarget> shiftTargets;
    for (auto* inst : exitBB->getInstructions()) {
        auto* lp = dyn_cast<PhiInst>(inst);
        if (!lp) break;
        if (!lp->getType()->isInt()) continue;

        // The exit phi's loop incoming(s) must agree on a single value v.
        Value* v = nullptr;
        bool consistent = true;
        for (int i = 0; i + 1 < lp->getNumOperands(); i += 2) {
            auto* src = dyn_cast<BasicBlock>(lp->getOperand(i + 1));
            if (!src || !L->has(src)) continue;
            if (!v) v = lp->getOperand(i);
            else if (v != lp->getOperand(i)) { consistent = false; break; }
        }
        if (!consistent || !v) continue;

        // v = recPhi >> k (Ashr) or recPhi / 2^k (Div).
        auto* bin = dyn_cast<BinaryInst>(v);
        if (!bin) continue;
        auto* recPhi = dyn_cast<PhiInst>(bin->getOperand(0));
        auto* kc = dyn_cast<ConstantInt>(bin->getOperand(1));
        if (!recPhi || !kc) continue;
        int shiftAmt = 0;
        if (bin->getOpID() == Instruction::Ashr) {
            shiftAmt = kc->getValue();
        } else if (bin->getOpID() == Instruction::Div) {
            int d = kc->getValue();
            if (d > 1 && (d & (d - 1)) == 0) shiftAmt = __builtin_ctz((unsigned)d);
        }
        if (shiftAmt <= 0 || !L->has(recPhi->getParent())) continue;

        // recPhi's latch update must be exactly v; its non-loop incoming is init.
        Value* init = nullptr;
        Value* recLatch = nullptr;
        for (int i = 0; i + 1 < recPhi->getNumOperands(); i += 2) {
            auto* src = dyn_cast<BasicBlock>(recPhi->getOperand(i + 1));
            if (!src) continue;
            if (L->has(src)) recLatch = recPhi->getOperand(i);
            else init = recPhi->getOperand(i);
        }
        if (!init || recLatch != v) continue;

        shiftTargets.push_back({lp, init, shiftAmt});
    }

    // Bound on |v|
    // Add overflow gating to the mul-add expressions in the mod-add closed expression.
    auto boundAbs = [&](Value* v, RangeAnalysis& ra, long& boundv) -> bool {
        if (auto* c = dyn_cast<ConstantInt>(v)) {
            long x = c->getValue();
            boundv = x < 0 ? -x : x;
            return true;
        }
        if (auto* ni = dyn_cast<Instruction>(v)) {
            if (ni->getOpID() == Instruction::Mod) {
                if (auto* k = dyn_cast<ConstantInt>(ni->getOperand(1))) {
                    long kk = k->getValue();
                    if (kk < 0) kk = -kk;
                    if (kk >= 1) {
                        boundv = kk - 1;
                        return true;
                    }
                }
            }
        }
        if (ra.has(v)) {
            IRange r = ra.get(v);
            if (r.low > INT_MIN && r.high < INT_MAX) {
                long lo = r.low < 0 ? -(long)r.low : r.low;
                long hi = r.high < 0 ? -(long)r.high : r.high;
                boundv = lo > hi ? lo : hi;
                return true;
            }
        }
        return false;
    };

    // Collect mod-add rec:  phi = (phi + C) % M.                                                              
    // The closed form is (init % M + C * iters) % M, gated so C * iters cannot overflow i32.  
    std::vector<ModAddTarget> modAddTargets;
    long startAbs = (ivStartC->val < 0) ? -ivStartC->val : ivStartC->val;
    long boundr = 0;
    bool canBound = boundAbs(info.rhs, ra, boundr);
    long itersBound = canBound ? (boundr + startAbs + 1) : 0;
    for (auto* inst : exitBB->getInstructions()) {
        auto* lp = dyn_cast<PhiInst>(inst);
        if (!lp) break;
        if (!lp->getType()->isInt()) continue;

        // The exit phi's loop incoming(s) must agree on a single value v.
        Value* v = nullptr;
        bool consistent = true;
        for (int i = 0; i + 1 < lp->getNumOperands(); i += 2) {
            auto* src = dyn_cast<BasicBlock>(lp->getOperand(i + 1));
            if (!src || !L->has(src)) continue;
            if (!v) v = lp->getOperand(i);
            else if (v != lp->getOperand(i)) { consistent = false; break; }
        }
        if (!consistent || !v) continue;

        // latch:                                                                                              
        //      %add = %x + C                                                                                  
        //      %x.next = %add % M                                                                             
        //      br ..., header, exit
        auto* mod = dyn_cast<BinaryInst>(v);
        if (!mod || mod->getOpID() != Instruction::Mod) continue;
        auto* mc = dyn_cast<ConstantInt>(mod->getOperand(1));
        auto* add = dyn_cast<BinaryInst>(mod->getOperand(0));
        if (!mc || mc->getValue() <= 0 || !add || add->getOpID() != Instruction::Add) continue;
        PhiInst* recPhi = nullptr;
        long C = 0;
        if (auto* p = dyn_cast<PhiInst>(add->getOperand(0))) {
            if (auto* c = dyn_cast<ConstantInt>(add->getOperand(1))) { recPhi = p; C = c->getValue(); }
        }
        if (!recPhi) {
            if (auto* p = dyn_cast<PhiInst>(add->getOperand(1))) {
                if (auto* c = dyn_cast<ConstantInt>(add->getOperand(0))) { recPhi = p; C = c->getValue(); }
            }
        }
        if (!recPhi || !L->has(recPhi->getParent())) continue;

        // recPhi's latch update must be exactly v; 
        // its non-loop incoming is init.
        Value* init = nullptr;
        Value* recLatch = nullptr;
        for (int i = 0; i + 1 < recPhi->getNumOperands(); i += 2) {
            auto* src = dyn_cast<BasicBlock>(recPhi->getOperand(i + 1));
            if (!src) continue;
            if (L->has(src)) recLatch = recPhi->getOperand(i);
            else init = recPhi->getOperand(i);
        }
        if (!init || recLatch != v) continue;

        long M = mc->getValue();
        long cc = C < 0 ? -C : C;
        // Overflow gate: |C| * iters + (M - 1) must stay within i32.
        if (!canBound) continue;
        if ((long long)cc * itersBound + (M - 1) > INT_MAX) continue;

        modAddTargets.push_back({lp, init, C, M});
    }

    bool prunedDead = false;
    bool didFold = false;

    std::vector<LinearTarget> liveLinearTargets;
    liveLinearTargets.reserve(linearTargets.size());
    for (auto& lt : linearTargets) {
        if (!hasRealUse(lt.lcssaPhi)) {
            lt.lcssaPhi->eraseInst();
            prunedDead = true;
        } else {
            liveLinearTargets.push_back(lt);
        }
    }
    linearTargets.swap(liveLinearTargets);

    std::vector<ShiftTarget> liveShiftTargets;
    liveShiftTargets.reserve(shiftTargets.size());
    for (auto& st : shiftTargets) {
        if (!hasRealUse(st.lcssaPhi)) {
            st.lcssaPhi->eraseInst();
            prunedDead = true;
        } else {
            liveShiftTargets.push_back(st);
        }
    }
    shiftTargets.swap(liveShiftTargets);

    std::vector<ModAddTarget> liveModAddTargets;
    liveModAddTargets.reserve(modAddTargets.size());
    for (auto& mt : modAddTargets) {
        if (!hasRealUse(mt.lcssaPhi)) {
            mt.lcssaPhi->eraseInst();
            prunedDead = true;
        } else {
            liveModAddTargets.push_back(mt);
        }
    }
    modAddTargets.swap(liveModAddTargets);

    if (linearTargets.empty() && shiftTargets.empty() && modAddTargets.empty())
        return prunedDead;

    auto& exitInsts = exitBB->getInstructions();
    auto insertPos = exitInsts.begin();
    while (insertPos != exitInsts.end() && isa<PhiInst>(*insertPos)) 
        ++insertPos;

    static int recID = 0;
    std::vector<Instruction*> emittedInsts;
    // Insert the newly generated closed solution instruction uniformly after the LCSSA Phi of the export block.
    auto emit = [&](Instruction* inst) -> Instruction* {
        inst->setName("rec" + std::to_string(recID++));
        inst->setParent(exitBB);
        exitInsts.insert(insertPos, inst);
        emittedInsts.push_back(inst);
        return inst;
    };

    auto guardWrap = [&](PhiInst* lp, Value* ranVal) -> Value* {
        Value* skipVal = nullptr;
        BasicBlock* ph = nullptr;
        for (int i = 0; i + 1 < lp->getNumOperands(); i += 2) {
            auto* src = dyn_cast<BasicBlock>(lp->getOperand(i + 1));
            if (src && !L->has(src)) { skipVal = lp->getOperand(i); ph = src; break; }
        }
        if (!ph || !skipVal) return ranVal; // dedicated exit: ranVal is the only value
        auto* gbr = dyn_cast<BranchInst>(ph->getInstructions().back());
        if (!gbr || gbr->getNumOperands() != 3) return nullptr; // unknown guard
        Value* cond = gbr->getOperand(0);
        bool ranWhenTrue = (dyn_cast<BasicBlock>(gbr->getOperand(1)) != exitBB);
        return emit(new SelectInst(cond, ranWhenTrue ? ranVal : skipVal, ranWhenTrue ? skipVal : ranVal, nullptr));
    };

    // Trip Count
    int adj = isDecreasing 
            ? (int)(ivStartC->val + (isSGE ? 1 : 0))
            : (int)(ivStartC->val - (isSLE ? 1 : 0));
    Value* tripCount;
    if (isDecreasing) {
        tripCount = emit(new BinaryInst(Instruction::Sub, new ConstantInt(adj), info.rhs, nullptr));
    } else {
        tripCount = (adj == 0) 
                ? info.rhs 
                : emit(new BinaryInst(Instruction::Sub, info.rhs, new ConstantInt(adj), nullptr));
    }
    if (absStep != 1) {
        tripCount = emit(new BinaryInst(Instruction::Add, tripCount, new ConstantInt(absStep - 1), nullptr));
        tripCount = emit(new BinaryInst(Instruction::Div, tripCount, new ConstantInt(absStep), nullptr));
    }

    //  Iters equal to the trip count + 1.
    Value* iters = (shiftTargets.empty() && modAddTargets.empty())
                ? nullptr
                : emit(new BinaryInst(Instruction::Add, tripCount, new ConstantInt(1), nullptr));

    // Expand the mathematical formulas analyzed by SCEV back to real IR instructions.
    std::function<Value*(SE*)> tryExpand = [&](SE* se) -> Value* {
        if (auto* c = dyn_cast<SEConst>(se))
            return new ConstantInt((int)c->val);
        if (auto* u = dyn_cast<SEUnknown>(se))
            return isLoopInvariantValue(u->v, L) ? u->v : nullptr;
        if (auto* m = dyn_cast<SEMul>(se)) {
            Value* base = tryExpand(m->base);
            if (!base) return nullptr;
            if (m->factor == 1) return base;
            return emit(new BinaryInst(Instruction::Mul, base, new ConstantInt((int)m->factor), nullptr));
        }
        if (auto* a = dyn_cast<SEAdd>(se)) {
            Value* acc = nullptr;
            for (auto* op : a->ops) {
                Value* v = tryExpand(op);
                if (!v) return nullptr;
                acc = acc ? emit(new BinaryInst(Instruction::Add, acc, v, nullptr)) : v;
            }
            return acc ? acc : static_cast<Value*>(new ConstantInt(0));
        }
        return nullptr;
    };

    // Handle linear inductive variables: 
    // Generate a closed solution (Start + TripCount * Step) and replace the use at the exit.
    for (auto& lt : linearTargets) {
        Value* startVal = tryExpand(lt.arStart);
        if (!startVal) continue;

        Value* exitVal;
        if (lt.arStep == 0) {
            exitVal = startVal;
        } else {
            Value* delta;
            if (lt.arStep == 1) {
                delta = tripCount;
            } else if (lt.arStep == -1) {
                delta = emit(new BinaryInst(Instruction::Sub, new ConstantInt(0), tripCount, nullptr));
            } else {
                delta = emit(new BinaryInst(Instruction::Mul, tripCount, new ConstantInt((int)lt.arStep), nullptr));
            }
            exitVal = emit(new BinaryInst(Instruction::Add, startVal, delta, nullptr));
        }

        Value* finalVal = guardWrap(lt.lcssaPhi, exitVal);
        if (!finalVal) continue; // unrecognized guard: leave the phi untouched
        bool replaced = replaceRealUsesWith(lt.lcssaPhi, finalVal);
        lt.lcssaPhi->eraseInst();
        didFold |= replaced;
    }

    // Handle shift inductive variables: exit = init >> min(31, iters * k).
    for (auto& st : shiftTargets) {
        int k = (int)st.shiftPerIter;
        Value* rawShift;
        if (k == 1) {
            rawShift = iters;
        } else if ((k & (k - 1)) == 0) {
            rawShift = emit(new BinaryInst(Instruction::Shl, iters, new ConstantInt(__builtin_ctz((unsigned)k)), nullptr));
        } else {
            rawShift = emit(new BinaryInst(Instruction::Mul, iters, new ConstantInt(k), nullptr));
        }
        auto* d = emit(new BinaryInst(Instruction::Sub, rawShift, new ConstantInt(31), nullptr));
        auto* sign = emit(new BinaryInst(Instruction::Ashr, d, new ConstantInt(31), nullptr));
        auto* masked = emit(new BinaryInst(Instruction::And, d, sign, nullptr));
        auto* posD = emit(new BinaryInst(Instruction::Sub, d, masked, nullptr));
        auto* clamped = emit(new BinaryInst(Instruction::Sub, rawShift, posD, nullptr));
        Value* exitVal = emit(new BinaryInst(Instruction::Ashr, st.init, clamped, nullptr));

        Value* finalVal = guardWrap(st.lcssaPhi, exitVal);
        if (!finalVal) continue; // unrecognized guard: leave the phi untouched
        bool replaced = replaceRealUsesWith(st.lcssaPhi, finalVal);
        st.lcssaPhi->eraseInst();
        didFold |= replaced;
    }

    // Modular-add recurrences:  exit = (init % M + C * iters) % M. At a guard-merge exit,
    // guardWrap selects on the loop entry guard so the skipped path keeps init.
    for (auto& mt : modAddTargets) {
        Value* im = emit(new BinaryInst(Instruction::Mod, mt.init, new ConstantInt((int)mt.M), nullptr));
        Value* cn = emit(new BinaryInst(Instruction::Mul, iters, new ConstantInt((int)mt.C), nullptr));
        Value* sum = emit(new BinaryInst(Instruction::Add, im, cn, nullptr));
        Value* closed = emit(new BinaryInst(Instruction::Mod, sum, new ConstantInt((int)mt.M), nullptr));

        Value* finalVal = guardWrap(mt.lcssaPhi, closed);
        if (!finalVal) continue; // unrecognized guard: leave the phi untouched
        bool replaced = replaceRealUsesWith(mt.lcssaPhi, finalVal);
        mt.lcssaPhi->eraseInst();
        didFold |= replaced;
    }

    bool hasLiveEmit = false;
    for (int i = (int)emittedInsts.size() - 1; i >= 0; --i) {
        auto* inst = emittedInsts[i];
        if (hasRealUse(inst)) {
            hasLiveEmit = true;
            continue;
        }
        inst->eraseInst();
    }

    didFold &= hasLiveEmit || emittedInsts.empty();
    return prunedDead || didFold;
}

bool LoopExitFold::deduplicateRec(Loop* L, SCEV& scev) {
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
        phis[k]->eraseInst();
    }
    return any;
}
