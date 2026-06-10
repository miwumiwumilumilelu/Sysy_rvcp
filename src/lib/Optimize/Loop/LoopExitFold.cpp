#include "../../../include/Optimize/Loop/LoopExitFold.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "../../../include/IR/Instruction.h"
#include <functional>
#include <utility>

using namespace sysy;

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

bool LoopExitFold::runOnLoop(Loop* L, Dominators& /*dt*/, SCEV& scev) {
    return deduplicateRec(L, scev);
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

    bool changed = false;
    std::function<void(Loop*)> visit = [&](Loop* L) {
        for (auto sub : L->sub) visit(sub);
        changed |= deduplicateRec(L, scev);
        changed |= foldExitValues(L, scev);
    };
    for (auto top : li.tops()) visit(top);
    return changed;
}

bool LoopExitFold::foldExitValues(Loop* L, SCEV& scev) {
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
    struct LinearTarget { 
        PhiInst* lcssaPhi; 
        SE* arStart; 
        int64_t arStep; 
    };
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

    // Collect all inductive variables affected by geometric decay (shift) and their internal states.
    struct ShiftTarget { 
        PhiInst* phi; 
        Value* initVal;
        BinaryInst* vNext;
        int64_t shiftPerIter;
    };
    std::vector<ShiftTarget> shiftTargets;
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        if (!phi->getType()->isInt()) continue;
        Value* initVal = nullptr;
        BinaryInst* vNext = nullptr;
        for (int i = 0; i + 1 < phi->getNumOperands(); i += 2) {
            auto* bb = cast<BasicBlock>(phi->getOperand(i + 1));
            if (L->has(bb)) 
                vNext = dyn_cast<BinaryInst>(phi->getOperand(i));
            else 
                initVal = phi->getOperand(i);
        }
        if (!initVal || !vNext) continue;
        auto* sr = dyn_cast<SEShiftRec>(scev.get(phi));
        if (sr && sr->loop == L)
            shiftTargets.push_back({phi, initVal, vNext, sr->shiftPerIter});
    }

    // Filter out shift inductive variables that are indeed used by LCSSA Phi in the exit block.
    struct ShiftWork { 
        ShiftTarget t; 
        std::vector<PhiInst*> phis;
    };
    std::vector<ShiftWork> shiftWork;
    for (auto& t : shiftTargets) {
        std::vector<PhiInst*> lcssaPhis;
        for (auto* ei : exitBB->getInstructions()) {
            auto* lp = dyn_cast<PhiInst>(ei);
            if (!lp) break;
            bool usesVNext = false, vNextOK = true, initOK = true;
            for (int i = 0; i + 1 < lp->getNumOperands(); i += 2) {
                auto* src = dyn_cast<BasicBlock>(lp->getOperand(i + 1));
                if (!src) continue;
                if (L->has(src)) {
                    if (lp->getOperand(i) == t.vNext) 
                        usesVNext = true;
                    else 
                        vNextOK = false;
                } else {
                    if (lp->getOperand(i) != t.initVal) 
                        initOK = false;
                }
            }
            if (usesVNext && vNextOK && initOK) 
                lcssaPhis.push_back(lp);
        }
        if (!lcssaPhis.empty()) 
            shiftWork.push_back({t, std::move(lcssaPhis)});
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

    std::vector<ShiftWork> liveShiftWork;
    liveShiftWork.reserve(shiftWork.size());
    for (auto& work : shiftWork) {
        std::vector<PhiInst*> livePhis;
        livePhis.reserve(work.phis.size());
        for (auto* lp : work.phis) {
            if (!hasRealUse(lp)) {
                lp->eraseInst();
                prunedDead = true;
            } else {
                livePhis.push_back(lp);
            }
        }
        if (!livePhis.empty()) {
            work.phis = std::move(livePhis);
            liveShiftWork.push_back(std::move(work));
        }
    }
    shiftWork.swap(liveShiftWork);

    if (linearTargets.empty() && shiftWork.empty()) 
        return prunedDead;

    auto& exitInsts = exitBB->getInstructions();
    auto insertPos = exitInsts.begin();
    while (insertPos != exitInsts.end() && isa<PhiInst>(*insertPos)) 
        ++insertPos;

    static int recID = 0;
    std::vector<Instruction*> emittedInsts;
    // Insert the newly generated closed solution instruction uniformly after the LCSSA Phi of the export block.
    auto emit = [&](BinaryInst* inst) -> BinaryInst* {
        inst->setName("rec" + std::to_string(recID++));
        inst->setParent(exitBB);
        exitInsts.insert(insertPos, inst);
        emittedInsts.push_back(inst);
        return inst;
    };

    // Trip Count
    int adj = isDecreasing ? (int)(ivStartC->val + (isSGE ? 1 : 0))
                        : (int)(ivStartC->val - (isSLE ? 1 : 0));
    Value* tripCount;
    if (isDecreasing) {
        tripCount = emit(new BinaryInst(Instruction::Sub, new ConstantInt(adj), info.rhs, nullptr));
    } else {
        tripCount = (adj == 0) ? info.rhs 
                            : emit(new BinaryInst(Instruction::Sub, info.rhs, new ConstantInt(adj), nullptr));
    }
    if (absStep != 1) {
        tripCount = emit(new BinaryInst(Instruction::Add, tripCount, new ConstantInt(absStep - 1), nullptr));
        tripCount = emit(new BinaryInst(Instruction::Div, tripCount, new ConstantInt(absStep), nullptr));
    }

    //  The number of times a shift operation is performed is usually equal to the trip count + 1.
    Value* iters = shiftWork.empty() ? nullptr 
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

        bool replaced = replaceRealUsesWith(lt.lcssaPhi, exitVal);
        lt.lcssaPhi->eraseInst();
        didFold |= replaced;
    }

    // Handle shift inductive variables:
    for (auto& [t, lcssaPhis] : shiftWork) {
        int k = (int)t.shiftPerIter;
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
        Value* exitVal = emit(new BinaryInst(Instruction::Ashr, t.initVal, clamped, nullptr));

        for (auto* lp : lcssaPhis) {
            bool replaced = replaceRealUsesWith(lp, exitVal);
            lp->eraseInst();
            didFold |= replaced;
        }

        if (!hasRealUse(t.vNext)) {
            t.vNext->eraseInst();

            for (int i = 0; i + 1 < t.phi->getNumOperands(); i += 2) {
                auto* bb = dyn_cast<BasicBlock>(t.phi->getOperand(i + 1));
                if (bb && L->has(bb)) { 
                    t.phi->setOperand(i, nullptr);
                    break; 
                }
            }
            if (!hasRealUse(t.phi)) {
                t.phi->eraseInst();
            }
        }
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
