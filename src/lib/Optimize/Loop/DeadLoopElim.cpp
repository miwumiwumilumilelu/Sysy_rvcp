#include "Optimize/Loop/DeadLoopElim.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/LoopInfo.h"
#include "Optimize/Analysis/PureFunc.h"
#include "Optimize/Analysis/SCEV.h"
#include "Optimize/Loop/LoopUtils/LoopDeletionUtils.h"
#include "Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include <map>
#include <set>

using namespace sysy;

// Compare replacement values by semantic equality.
static bool sameValue(Value* a, Value* b, SCEV& scev) {
    if (a == b) return true;
    if (auto* ca = dyn_cast<ConstantInt>(a))
        if (auto* cb = dyn_cast<ConstantInt>(b))
            return ca->getValue() == cb->getValue();
    if (auto* ca = dyn_cast<ConstantFloat>(a))
        if (auto* cb = dyn_cast<ConstantFloat>(b))
            return ca->getValue() == cb->getValue();
    return scev.equal(scev.get(a), scev.get(b));
}

// Find the unique loop entry predecessor after rotation.
static BasicBlock* findPreh(Loop* L, Dominators& dt) {
    if (!L || !L->head) return nullptr;

    BasicBlock* entryPred = nullptr;
    for (auto* pred : dt.getPredecessors(L->head)) {
        if (L->has(pred)) continue;
        // Prevent multiple preheader.
        if (entryPred) return nullptr;
        entryPred = pred;
    }
    return entryPred;
}

static BasicBlock* findSingleExit(Loop* L) {
    if (!L || L->exits.size() != 1) return nullptr;
    return L->exits[0];
}

// Only one way is acceptable to use a def defined inside a loop outside the loop:
// It first flows to the phi outside the loop, and this phi has already been handled by handledPhis(exitBB).
static bool hasUnhandledLiveOutUses(Loop* L, 
                                    const std::map<PhiInst*, Value*>& handledPhis) {
    for (auto* bb : L->blocks) {
        for (auto* def : bb->getInstructions()) {
            if (def->getType()->isVoid()) continue;
            for (auto* user : def->getUsers()) {
                auto* userInst = dyn_cast<Instruction>(user);
                // Conservative
                if (!userInst) return true;

                if (auto* phi = dyn_cast<PhiInst>(userInst)) {
                    if (!L->has(phi->getParent())) {
                        if (!phi->getUsers().empty() && !handledPhis.count(phi))
                            return true;
                    }
                } else {
                    if (!L->has(userInst->getParent()))
                        return true;
                }
            }
        }
    }
    return false;
}

static bool isEntrySkipEdgeNeverTaken(
    Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev);

static bool hasObservableSideEffects(Loop* L) {
    std::unordered_map<Function*, bool> purity;
    for (auto* bb : L->blocks) {
        for (auto* inst : bb->getInstructions()) {
            if (isa<StoreInst>(inst)) return true;
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isPureFunc(call->getFunction(), purity)) return true;
        }
    }
    return false;
}

// Collect exit-phi replacements for loops whose outgoing value is stable.
static bool collectExitPhiReplacements(
    Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev,
    std::map<PhiInst*, Value*>& outMap) {
    if (!L || !exitBB) return false;

    auto* entryPred = findPreh(L, dt);
    bool skipEdgeNeverTaken = isEntrySkipEdgeNeverTaken(L, exitBB, dt, scev);

    // Build replacement values for live exit phis.
    for (auto* inst : exitBB->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break; // Phi nodes stay at the block front.

        if (phi->getUsers().empty()) continue; // Ignore dead exit phis.

        Value* uniqueLoopVal = nullptr;
        bool hasLoopIncoming = false;

        // All loop incomings must agree on one value.
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (!fromBB || !L->has(fromBB)) continue;
            Value* val = phi->getOperand(k);
            hasLoopIncoming = true;
            if (!uniqueLoopVal) {
                uniqueLoopVal = val;
            } else if (!sameValue(uniqueLoopVal, val, scev)) {
                return false; // Reject loops with multiple outgoing values.
            }
        }

        if (!hasLoopIncoming) continue; // Ignore phis unrelated to this loop.

        // The replacement value must remain available after deletion.
        if (!isDeletionMaterializable(uniqueLoopVal, L))
            return false;

        // Non-loop incomings must match the loop result.
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (!fromBB || !L->has(fromBB)) continue;
            if (skipEdgeNeverTaken && fromBB == entryPred) continue;
            if (!sameValue(uniqueLoopVal, phi->getOperand(k), scev))
                return false;
        }

        outMap[phi] = uniqueLoopVal;
    }

    // Reject remaining loop-defined values that still escape the loop.
    return !hasUnhandledLiveOutUses(L, outMap);
}

static bool isEntrySkipEdgeNeverTaken(
    Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev) {
    if (!L || !exitBB) return false;
    auto* entryPred = findPreh(L, dt);
    if (!entryPred || entryPred->getInstructions().empty()) return false;

    auto* br = dyn_cast<BranchInst>(entryPred->getInstructions().back());
    if (!br || br->getNumOperands() != 3) return false;

    auto* t = dyn_cast<BasicBlock>(br->getOperand(1));
    auto* f = dyn_cast<BasicBlock>(br->getOperand(2));
    if (!t || !f) return false;

    bool trueIsExit = (t == exitBB);
    bool falseIsExit = (f == exitBB);
    if (trueIsExit == falseIsExit) return false;

    int condVal = evaluateDeletionCond(br->getOperand(0), scev);
    if (condVal < 0) return false;
    bool takesExit = trueIsExit ? (condVal != 0) : (condVal == 0);
    return !takesExit;
}

static bool breakBackedgeIfNotTaken(Loop* L, BasicBlock* exitBB, Dominators& dt) {
    if (!L || !exitBB || !L->head || !L->latch) return false;
    auto* entryPred = findPreh(L, dt);
    if (!entryPred) return false;

    auto& latchInsts = L->latch->getInstructions();
    if (latchInsts.empty()) return false;
    auto* br = dyn_cast<BranchInst>(latchInsts.back());
    if (!br || br->getNumOperands() != 3) return false;

    auto* trueSucc = dyn_cast<BasicBlock>(br->getOperand(1));
    auto* falseSucc = dyn_cast<BasicBlock>(br->getOperand(2));
    if (!trueSucc || !falseSucc) return false;
    bool trueIsHead = (trueSucc == L->head);
    bool falseIsHead = (falseSucc == L->head);
    if (trueIsHead == falseIsHead) return false;
    if ((trueIsHead ? falseSucc : trueSucc) != exitBB) return false;

    // Fold header phis to their entry values before cutting the backedge.
    std::vector<std::unique_ptr<ConstantInt>> tempOwner;
    std::unordered_map<Value*, Value*> cache;
    std::set<Value*> vis;
    auto* cond = dyn_cast<ConstantInt>(
        evaluateFirstIterValue(br->getOperand(0), L, entryPred, dt, tempOwner, cache, vis));
    if (!cond) return false;

    bool takesHead = trueIsHead ? (cond->getValue() != 0) : (cond->getValue() == 0);
    if (takesHead) return false;

    auto& headInsts = L->head->getInstructions();
    std::vector<PhiInst*> phis;
    for (auto* inst : headInsts) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        phis.push_back(phi);
    }
    for (auto* phi : phis) {
        Value* initVal = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (fromBB == entryPred) {
                initVal = phi->getOperand(k);
                break;
            }
        }
        if (!initVal) return false;
        phi->replaceAllUsesWith(initVal);
    }
    for (auto* phi : phis) {
        for (int i = 0; i < phi->getNumOperands(); ++i) phi->setOperand(i, nullptr);
        phi->setParent(nullptr);
        headInsts.remove(phi);
        delete phi;
    }

    latchInsts.pop_back();
    br->replaceAllUsesWith(nullptr);
    for (int i = 0; i < br->getNumOperands(); ++i) br->setOperand(i, nullptr);
    br->setParent(nullptr);
    delete br;
    new BranchInst(exitBB, L->latch);
    return true;
}

static bool collectZeroTrip(Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev,
                                            std::map<PhiInst*, Value*>& outMap) {
    if (!L || !exitBB) return false;
    auto* entryPred = findPreh(L, dt);
    if (!entryPred || entryPred->getInstructions().empty()) return false;

    // After looprotate, the preheader br is cond jmp.
    auto* br = dyn_cast<BranchInst>(entryPred->getInstructions().back());
    if (!br || br->getNumOperands() != 3) return false;

    // Check if one branch to exit and the other to header.
    auto* t = dyn_cast<BasicBlock>(br->getOperand(1));
    auto* f = dyn_cast<BasicBlock>(br->getOperand(2));
    if (!t || !f) return false;
    bool trueIsExit = (t == exitBB);
    bool falseIsExit = (f == exitBB);
    if (trueIsExit == falseIsExit) return false;

    int condVal = evaluateDeletionCond(br->getOperand(0), scev);
    if (condVal < 0) return false;
    bool takesExit = trueIsExit ? (condVal != 0) : (condVal == 0);
    // if the edge to exit is not taken, the loop executes at least once, so we cannot delete it.
    if (!takesExit) return false;

    for (auto* inst : exitBB->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        if (phi->getUsers().empty()) continue;

        Value* fromEntry = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (fromBB == entryPred) {
                fromEntry = phi->getOperand(k);
                break;
            }
        }
        if (!fromEntry) return false;
        outMap[phi] = fromEntry;
    }

    // Check if there are other live-out values that we cannot handle.
    return !hasUnhandledLiveOutUses(L, outMap);
}

static bool isKnownToTerminate(Loop* L, SCEV& scev) {
    return hasKnownFiniteTripCount(L, scev);
}

// Rewrite exit users before redirecting the entry edge.
static bool performDeletion(Loop* L, BasicBlock* exitBB, 
                            std::map<PhiInst*, Value*>& replacements,
                            Dominators& dt) {
    if (!exitBB) return false;
    auto* entryPred = findPreh(L, dt);
    if (!entryPred) return false;

    auto& entryInsts = entryPred->getInstructions();
    if (entryInsts.empty()) return false;

    // entryPred:
    //      br ... head/exitBB; 
    // loop:
    //      %x.loop = ...; 
    // exitBB: 
    //      %x.exit = phi [ %x.loop, latch ], ...; 
    // out: 
    //      use %x.exit
    //
    // becomes:
    //
    // entryPred: 
    //      %x.dle = ...; 
    //      br exitBB; 
    // exitBB: 
    //      ; 
    // out: 
    //      use %x.dle
    std::unordered_map<Value*, Value*> matCache;
    for (auto& [phi, val] : replacements) {
        std::set<Value*> vis;
        Value* repl = materializeForDeletion(val, L, entryPred, matCache, vis);
        if (!repl) return false;
        phi->replaceAllUsesWith(repl);
        for (int i = 0; i < phi->getNumOperands(); i++) phi->setOperand(i, nullptr);
        phi->setParent(nullptr);
        exitBB->getInstructions().remove(phi);
        delete phi;
    }

    // Delete dead exit phis that still mention the loop.
    // such as:
    // y.exit = phi [ y.loop, latch ], [ y.entry, entryPred ]
    // but y.exit is not used outside, we can just delete it without replacement.
    {
        auto& exitInsts = exitBB->getInstructions();
        auto it = exitInsts.begin();
        while (it != exitInsts.end()) {
            auto* phi = dyn_cast<PhiInst>(*it);
            if (!phi) break;
            bool hasLoopIncoming = false;
            for (int k = 1; k < (int)phi->getNumOperands(); k += 2)
                if (auto* src = dyn_cast<BasicBlock>(phi->getOperand(k)))
                    if (L->has(src)) { hasLoopIncoming = true; break; }
            if (hasLoopIncoming && phi->getUsers().empty()) {
                for (int i = 0; i < phi->getNumOperands(); i++) phi->setOperand(i, nullptr);
                phi->setParent(nullptr);
                it = exitInsts.erase(it);
                delete phi;
            } else {
                ++it;
            }
        }
    }

    // Redirect the unique entry edge to the exit block.
    auto* entryTerm = entryInsts.back();
    entryInsts.pop_back();
    entryTerm->replaceAllUsesWith(nullptr);
    for (int i = 0; i < entryTerm->getNumOperands(); i++) entryTerm->setOperand(i, nullptr);
    entryTerm->setParent(nullptr);
    delete entryTerm;

    new BranchInst(exitBB, entryPred);
    return true;
}

static bool runOnFunction(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;

    bool changed = false;
    bool progress;
    do {
        progress = false;
        Dominators dt(f);
        dt.run();
        LoopInfo li(f, dt);
        SCEV scev(f, li);

        // Visit inner loops first.
        std::function<bool(Loop*)> visit = [&](Loop* L) -> bool {
            for (auto* sub : L->sub)
                if (visit(sub)) return true;

            // Require a single latch and recover entry from dominators.
            if (L->latches.size() != 1) return false;

            // Stay conservative while subloops remain.
            if (!L->sub.empty()) return false;

            // Allow multiple exiting blocks, but require one exit block.
            auto* exitBB = findSingleExit(L);
            if (!exitBB) return false;

            // Remove loops that are provably skipped on entry.
            {
                std::map<PhiInst*, Value*> replacements;
                if (collectZeroTrip(L, exitBB, dt, scev, replacements))
                    return performDeletion(L, exitBB, replacements, dt);
            }

            // Remove finite side-effect-free loops with stable exit values.
            if (!hasObservableSideEffects(L)) {
                std::map<PhiInst*, Value*> replacements;
                if (collectExitPhiReplacements(L, exitBB, dt, scev, replacements)) {
                    if (isKnownToTerminate(L, scev) &&
                        performDeletion(L, exitBB, replacements, dt))
                        return true;
                }
            }

            // Keep the first iteration and cut the backedge when possible.
            if (breakBackedgeIfNotTaken(L, exitBB, dt))
                return true;

            return false;
        };

        for (auto* top : li.tops())
            if (visit(top)) { progress = true; changed = true; break; }
    } while (progress);

    return changed;
}

bool DeadLoopElim::run() {
    bool any = false;
    for (auto* f : M->getFunctions())
        any |= runOnFunction(f);
    return any;
}
