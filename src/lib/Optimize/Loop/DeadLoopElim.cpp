#include "../../../include/Optimize/Loop/DeadLoopElim.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/LoopInfo.h"
#include "../../../include/Optimize/Analysis/PureFunc.h"
#include "../../../include/Optimize/Analysis/SCEV.h"
#include "../../../include/Optimize/Analysis/ValueTracking.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopCloneUtils.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopDeletionUtils.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include <map>
#include <set>
#include <functional>

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

// Redundant-repeat-loop collapse
static bool isDeadPhiClosure(PhiInst* phi) {
    std::set<Value*> seen{phi};
    std::vector<Value*> work{phi};
    while (!work.empty()) {
        Value* v = work.back();
        work.pop_back();
        for (auto* user : v->getUsers()) {
            if (!isa<PhiInst>(user)) return false; // reaches a real use
            if (seen.insert(user).second) work.push_back(user);
        }
    }
    return true;
}

static bool isBadPhi(PhiInst* phi, Loop* L, BasicBlock* preHeader) {
    Value* init = nullptr;
    Value* backVal = nullptr;
    for (int k = 0; k < phi->getNumOperands(); k += 2) {
        auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
        if (fromBB == preHeader) init = phi->getOperand(k);
        else if (fromBB && L->has(fromBB)) backVal = phi->getOperand(k);
    }
    if (init && backVal && init == backVal) return true; // loop-invariant PHI
    return isDeadPhiClosure(phi); // or dead PHIs
}

static bool isControlOnlyIV(PhiInst* phi, Loop* L, BasicBlock* preHeader) {
    // iv = phi [init, entry], [iv.next, latch]
    if (phi->getNumOperands() != 4) return false; // exactly entry + latch incomings
    Value* inc = nullptr;
    for (int k = 0; k < phi->getNumOperands(); k += 2) {
        auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
        // Find the latch.
        if (fromBB && fromBB != preHeader && L->has(fromBB))
            inc = phi->getOperand(k);
    }
    auto* bin = dyn_cast<BinaryInst>(inc);
    // Only support AddInstruction.
    if (!bin || bin->getOpID() != Instruction::Add) return false;
    bool selfInc = (bin->getOperand(0) == phi && isa<ConstantInt>(bin->getOperand(1))) ||
                   (bin->getOperand(1) == phi && isa<ConstantInt>(bin->getOperand(0)));
    if (!selfInc) return false;

    std::set<Value*> ctrlCond;
    for (BasicBlock* bb : {L->head, L->latch}) {
        if (!bb || bb->getInstructions().empty()) continue;
        if (auto* br = dyn_cast<BranchInst>(bb->getInstructions().back()))
            // Such as br cmp, body, exit, get the cmp value.
            if (br->getNumOperands() == 3)
                ctrlCond.insert(br->getOperand(0));
    }

    auto allowed = [&](Value* uv, Value* selfOk) {
        if (uv == selfOk) return true; // own increment / back-edge phi
        if (ctrlCond.count(uv)) return true; // used as loop-control cmp
        return isa<ICmpInst>(uv) && uv->getUsers().empty(); // dead cmp, which may occur after loop-rotate.
    };

    for (auto* u : phi->getUsers())
        if (!allowed(u, inc)) return false;
    for (auto* u : inc->getUsers())
        if (!allowed(u, phi)) return false;
    return true;
}

// Recursive lower-bound proof that a SCEV is provably >= 0.
static bool scevNonNeg(SE* s, ValueTracking& vt) {
    if (!s) return false;
    if (auto* c = dyn_cast<SEConst>(s)) return c->val >= 0;
    if (auto* ar = dyn_cast<SEAddRec>(s)) return ar->step >= 0 && scevNonNeg(ar->start, vt);
    if (auto* m = dyn_cast<SEMul>(s)) return m->factor >= 0 && scevNonNeg(m->base, vt);
    if (auto* a = dyn_cast<SEAdd>(s)) {
        for (auto* op : a->ops) 
            if (!scevNonNeg(op, vt)) return false;
        return true;
    }
    if (auto* u = dyn_cast<SEUnknown>(s)) return vt.isNonNeg(u->v);
    return false;
}

// Walk a decay-then-index GEP chain: addr = base[0][i0][i1]...[im-1].
// Fills base and idxs=[i0, i1, ..., im-1].
static bool recoverGEPIndices(Value* addr, Value*& base, std::vector<Value*>& idxs) {
    std::vector<Value*> rev;
    Value* cur = addr;
    while (auto* gep = dyn_cast<GetElementPtrInst>(cur)) {
        rev.push_back(gep->getOperand(1));
        cur = gep->getOperand(0);
    }
    if (rev.empty() || isa<GetElementPtrInst>(cur)) return false;
    base = cur;
    auto* decay = dyn_cast<ConstantInt>(rev.back());
    // base[0][i0][i1]...
    if (!decay || decay->getValue() != 0) return false;
    rev.pop_back();
    if (rev.empty()) return false;
    idxs.assign(rev.rbegin(), rev.rend());
    return true;
}

// An upper bound recovered from a loop's exit test.
struct UpperBound {
    Value* val = nullptr;
    bool inclusive = false;
    bool ok() const {
        return val != nullptr;
    }
};

// Recover idx's upper bound from loop L's exit test. 
static UpperBound extractUpperBound(Loop* L, Value* idx, SCEV& scev) {
    if (!L) return {};
    for (BasicBlock* tb : {L->latch, L->head}) {
        if (!tb) continue;
        ExitBranchInfo info;
        if (!analyzeExitBranch(L, tb, scev, info) || !info.cmp) continue;
        bool inclusive;
        if (info.continuePred == ICmpInst::SLT) inclusive = false;
        else if (info.continuePred == ICmpInst::SLE) inclusive = true;
        else continue;

        if (!(info.lhsRec && info.lhsRec->loop == L && !info.rhsRec)) continue;
        Value* tested = info.lhs;
        Value* bound = info.rhs;
        if (!tested || !bound) continue;
        bool ok = (tested == idx);
        if (!ok)
            if (auto* add = dyn_cast<BinaryInst>(tested))
                if (add->getOpID() == Instruction::Add) {
                    auto chk = [&](Value* a, Value* b) {
                        auto* c = dyn_cast<ConstantInt>(b);
                        return a == idx && c && c->getValue() >= 0;
                    };
                    ok = chk(add->getOperand(0), add->getOperand(1)) ||
                         chk(add->getOperand(1), add->getOperand(0));
                }
        if (ok) return {bound, inclusive};
    }
    return {};
}

static bool limitEqual(const UpperBound& a, const UpperBound& b, SCEV& scev) {
    int oa = a.inclusive ? 1 : 0;
    int ob = b.inclusive ? 1 : 0;
    if (sameValue(a.val, b.val, scev)) return oa == ob;
    auto* ca = dyn_cast<ConstantInt>(a.val);
    auto* cb = dyn_cast<ConstantInt>(b.val);
    return ca && cb && (ca->getValue() + oa) == (cb->getValue() + ob);
}

static bool limitLE(const UpperBound& a, const UpperBound& b, SCEV& scev) {
    int oa = a.inclusive ? 1 : 0;
    int ob = b.inclusive ? 1 : 0;
    if (sameValue(a.val, b.val, scev)) return oa <= ob;
    auto* ca = dyn_cast<ConstantInt>(a.val);
    auto* cb = dyn_cast<ConstantInt>(b.val);
    return ca && cb && (ca->getValue() + oa) <= (cb->getValue() + ob);
}

// idx provably in [0, U).
static bool provesInRange(Value* idx, const UpperBound& U, SCEV& scev, ValueTracking& vt) {
    // idx >= 0
    if (!scevNonNeg(scev.get(idx), vt)) return false;
    auto* ar = dyn_cast<SEAddRec>(scev.get(idx));
    if (!ar || ar->step < 1) return false;
    // idx < U
    UpperBound b = extractUpperBound(ar->loop, idx, scev);
    return b.ok() && limitEqual(b, U, scev);
}

// Check if val transitively load from base object.
// such as B[i][j] = B[i][j] + 1;
// New B rely on old B.
static bool valueLoadsBase(Value* val, Value* base, std::set<Value*>& seen) {
    if (!val || !seen.insert(val).second) return false;
    if (auto* ld = dyn_cast<LoadInst>(val)) {
        Value* b = nullptr;
        std::vector<Value*> idx;
        if (recoverGEPIndices(ld->getOperand(0), b, idx) && b == base) return true;
    }
    auto* inst = dyn_cast<Instruction>(val);
    if (!inst) return false;
    for (int i = 0; i < inst->getNumOperands(); i++)
        if (valueLoadsBase(inst->getOperand(i), base, seen)) return true;
    return false;
}

static bool writesEveryIteration(BasicBlock* stBB, Loop* L, Dominators& dt) {
    Loop* cur = nullptr;
    std::function<void(Loop*)> dive = [&](Loop* lp) {
        for (auto* sub : lp->sub) {
            if (sub->has(stBB)) { 
                cur = sub; 
                dive(sub); 
                return; 
            }
        }
    };
    dive(L);

    if (!cur || !cur->latch || !dt.dominates(stBB, cur->latch)) return false;
    while (cur->up && cur->up != L) {
        Loop* parent = cur->up;
        if (!cur->pre || !parent->latch) return false;
        if (!dt.dominates(cur->pre, parent->latch)) return false;
        cur = parent;
    }
    return true;
}

// Store footprint covers a load footprint: same rank and every dimension matches.
static bool footprintCovers(const std::vector<Value*>& store,
                            const std::vector<Value*>& load, SCEV& scev) {
    // Here store/load is dim.
    // such as:
    // B[i][j] = 0;
    // x = B[p][q];
    // 
    // store = [i, j]  load  = [p, q]
    if (store.size() != load.size()) return false;

    // Whether the range of indices swept by a `store` operation along a specific dimension covers 
    // the range of indices to be read by a `load` operation along the same dimension.
    // Ensure that all loads in the current loop originate from stores,
    // rather than from stale values ​​from the previous loop iteration.
    auto indexFootprintMatch = [&](Value* a, Value* b, SCEV& scev) -> bool { // a=load, b=store
        if (sameValue(a, b, scev)) return true;

        auto scevLE = [&](SE* lo, SE* hi, SCEV& scev) -> bool{
            if (scev.equal(lo, hi)) return true;
            auto* cl = dyn_cast<SEConst>(lo);
            auto* ch = dyn_cast<SEConst>(hi);
            return cl && ch && cl->val <= ch->val;
        };

        auto* ra = dyn_cast<SEAddRec>(scev.get(a));
        auto* rb = dyn_cast<SEAddRec>(scev.get(b));
        if (!ra || !rb || ra->step != rb->step || rb->step < 1) return false;
        if (rb->step == 1) {
            if (!scevLE(rb->start, ra->start, scev)) return false; // store starts <= load
        } else {
            if (!scev.equal(rb->start, ra->start)) return false; // aligned strided grid
        }
        UpperBound ba = extractUpperBound(ra->loop, a, scev); // load upper bound
        UpperBound bb = extractUpperBound(rb->loop, b, scev); // store upper bound
        return ba.ok() && bb.ok() && limitLE(ba, bb, scev); // load range <= store range
    };

    for (size_t d = 0; d < store.size(); d++)
        if (!indexFootprintMatch(load[d], store[d], scev)) return false;
    return true;
}

// One array memory access inside the loop body.
struct MemAcc { 
    Instruction* inst;
    BasicBlock* bb; 
    std::vector<Value*> idxs; 
    bool isStore; 
};

static bool storeBeforeLoadNest(const MemAcc& st, const MemAcc& ld, Loop* L, Dominators& dt) {
    // `a` executes before `b` on every iteration.
    auto instBefore = [&](Instruction* a, Instruction* b, Dominators& dt) -> bool {
        if (a->getParent() == b->getParent()) {
            for (auto* inst : a->getParent()->getInstructions()) {
                if (inst == a) return true;
                if (inst == b) return false;
            }
            return false;
        }
        return dt.dominates(a->getParent(), b->getParent());
    };

    if (instBefore(st.inst, ld.inst, dt)) return true;
    Loop* Rtop = nullptr;
    for (auto* sub : L->sub) 
        if (sub->has(st.bb)) { 
            Rtop = sub; 
            break; 
        }
    return Rtop && Rtop->exits.size() == 1 && dt.dominates(Rtop->exits[0], ld.bb) && writesEveryIteration(st.bb, L, dt);
}

// A specific array or object, `base`, is completely reset at the beginning of the current round;
// all subsequent accesses to it occur within the scope of this reset,
// so there is no dependency on stale values ​​from the previous round.
static bool provesFullBoxReset(Value* base, std::vector<MemAcc>& accs, Loop* L,
                               Dominators& dt, SCEV& scev, ValueTracking& vt) {
    MemAcc* reset = nullptr;
    std::vector<UpperBound> U;
    for (auto& a : accs) {
        if (!a.isStore) continue;
        std::vector<UpperBound> bounds;
        std::set<Value*> ivSet;
        bool ok = true;
        for (auto* idx : a.idxs) {
            auto* ar = dyn_cast<SEAddRec>(scev.get(idx));
            if (!ar || ar->step != 1) { ok = false; break; }
            auto* s0 = dyn_cast<SEConst>(ar->start);
            if (!s0 || s0->val != 0) { ok = false; break; } // start at 0
            if (!ivSet.insert(idx).second) { ok = false; break; } // distinct dims
            UpperBound bd = extractUpperBound(ar->loop, idx, scev);
            if (!bd.ok()) { ok = false; break; }
            if (auto* bi = dyn_cast<Instruction>(bd.val))
                if (L->has(bi->getParent())) { ok = false; break; } // box dim invariant w.r.t. L
            bounds.push_back(bd);
        }
        if (!ok) continue;
        std::set<Value*> seen;
        if (valueLoadsBase(cast<StoreInst>(a.inst)->getOperand(0), base, seen)) continue;
        // The reset must write the whole box unconditionally, not under a
        // data-dependent condition (else only some cells are cleared).
        if (!writesEveryIteration(a.bb, L, dt)) continue;
        reset = &a;
        U = bounds;
        break;
    }
    if (!reset) return false;
    Loop* Rtop = nullptr;
    for (auto* sub : L->sub) if (sub->has(reset->bb)) { Rtop = sub; break; }
    if (!Rtop || Rtop->exits.size() != 1) return false;
    BasicBlock* resetExit = Rtop->exits[0];
    for (auto& a : accs) {
        if (&a == reset) continue;
        if (!dt.dominates(resetExit, a.bb)) return false;
        if (a.idxs.size() != U.size()) return false;
        for (size_t d = 0; d < a.idxs.size(); d++)
            if (!provesInRange(a.idxs[d], U[d], scev, vt)) return false;
    }
    return true;
}

// For each array in the loop, determine whether the value read in the current iteration depends on content 
// left over from the previous iteration.
static bool provesWriteBeforeRead(std::vector<MemAcc>& accs, Loop* L, Dominators& dt, SCEV& scev) {
    for (auto& ld : accs) {
        if (ld.isStore) continue;
        bool covered = false;
        for (auto& st : accs) {
            if (!st.isStore) continue;
            if (footprintCovers(st.idxs, ld.idxs, scev) && storeBeforeLoadNest(st, ld, L, dt)) {
                covered = true;
                break;
            }
        }
        if (!covered) return false;
    }
    return true;
}

// There are no cross-iteration memory dependencies in the entire loop L.
static bool noLoopCarriedMemDep(Loop* L, Dominators& dt, SCEV& scev) {
    auto* f = L->head ? L->head->getParentFunc() : nullptr;
    if (!f) return false;
    ValueTracking vt(f);

    std::map<Value*, std::vector<MemAcc>> byBase;
    for (auto* bb : L->blocks) {
        for (auto* inst : bb->getInstructions()) {
            Value* addr = nullptr;
            bool isStore = false;
            if (auto* st = dyn_cast<StoreInst>(inst)) { 
                addr = st->getOperand(1); 
                isStore = true; 
            }
            else if (auto* ld = dyn_cast<LoadInst>(inst)) { addr = ld->getOperand(0); }
            else continue;
            Value* base = nullptr;
            std::vector<Value*> idxs;
            if (!recoverGEPIndices(addr, base, idxs)) return false; // unrecoverable -> bail
            byBase[base].push_back({inst, bb, std::move(idxs), isStore});
        }
    }

    for (auto& [base, accs] : byBase) {
        bool anyStore = false;
        for (auto& a : accs) if (a.isStore) { 
            anyStore = true;
            break; 
        }
        if (!anyStore) continue; // read-only base
        if (provesWriteBeforeRead(accs, L, dt, scev)) continue;
        if (provesFullBoxReset(base, accs, L, dt, scev, vt)) continue;
        return false;
    }
    return true;
}

// Evaluate whether the preheader's conditional branch takes the exit edge.
// Returns +1 = always takes exit (zero-trip), -1 = never takes exit (skip-edge invariant), 0 = unknown.
static int evalPreheaderEdge(Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev) {
    if (!L || !exitBB) return 0;
    auto* preHeader = L->entryBlock(dt);
    if (!preHeader || preHeader->getInstructions().empty()) return 0;
    auto* br = dyn_cast<BranchInst>(preHeader->getInstructions().back());
    if (!br || br->getNumOperands() != 3) return 0;
    auto* t = dyn_cast<BasicBlock>(br->getOperand(1));
    auto* f = dyn_cast<BasicBlock>(br->getOperand(2));
    if (!t || !f) return 0;
    bool trueIsExit = (t == exitBB);
    bool falseIsExit = (f == exitBB);
    if (trueIsExit == falseIsExit) return 0;
    int condVal = evaluateDeletionCond(br->getOperand(0), scev);
    if (condVal < 0) return 0;
    bool takesExit = trueIsExit ? (condVal != 0) : (condVal == 0);
    return takesExit ? 1 : -1;
}

// Collect exit-phi replacements for loops whose outgoing value is stable.
static bool collectExitPhiReplacements(
    Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev,
    std::map<PhiInst*, Value*>& outMap) {
    if (!L || !exitBB) return false;

    auto* preHeader = L->entryBlock(dt);
    bool skipEdgeNeverTaken = (evalPreheaderEdge(L, exitBB, dt, scev) == -1);

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

        // Only the entry skip edge may contribute a loop-outside incoming.
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (!fromBB || L->has(fromBB)) continue;
            if (fromBB != preHeader) return false;
            if (skipEdgeNeverTaken) continue;
            if (!sameValue(uniqueLoopVal, phi->getOperand(k), scev))
                return false;
        }

        outMap[phi] = uniqueLoopVal;
    }

    // Reject remaining loop-defined values that still escape the loop.
    return !hasUnhandledLiveOutUses(L, outMap);
}

static bool breakBackedgeIfNotTaken(Loop* L, BasicBlock* exitBB, Dominators& dt) {
    if (!L || !exitBB || !L->head || !L->latch) return false;
    auto* preHeader = L->entryBlock(dt);
    if (!preHeader) return false;

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
        evaluateFirstIterValue(br->getOperand(0), L, preHeader, dt, tempOwner, cache, vis));
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
            if (fromBB == preHeader) {
                initVal = phi->getOperand(k);
                break;
            }
        }
        if (!initVal) return false;
        phi->replaceAllUsesWith(initVal);
    }
    for (auto* phi : phis) {
        phi->eraseInst();
    }

    br->replaceAllUsesWith(nullptr);
    br->eraseInst();
    new BranchInst(exitBB, L->latch);
    return true;
}

static bool collectZeroTrip(Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev,
                                            std::map<PhiInst*, Value*>& outMap) {
    if (evalPreheaderEdge(L, exitBB, dt, scev) != 1) return false;

    auto* preHeader = L->entryBlock(dt);
    for (auto* inst : exitBB->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        if (phi->getUsers().empty()) continue;

        Value* fromPreH = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (fromBB == preHeader) {
                fromPreH = phi->getOperand(k);
                break;
            }
        }
        if (!fromPreH) return false;
        outMap[phi] = fromPreH;
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
    auto* preHeader = L->entryBlock(dt);
    if (!preHeader) return false;

    auto& entryInsts = preHeader->getInstructions();
    if (entryInsts.empty()) return false;

    // preHeader:
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
    // preHeader: 
    //      %x.dle = ...; 
    //      br exitBB; 
    // exitBB: 
    //      ; 
    // out: 
    //      use %x.dle
    std::unordered_map<Value*, Value*> matCache;
    for (auto& [phi, val] : replacements) {
        std::set<Value*> vis;
        Value* repl = materializeForDeletion(val, L, preHeader, matCache, vis);
        if (!repl) return false;
        phi->replaceAllUsesWith(repl);
        phi->eraseInst();
    }

    // Delete dead exit phis that still mention the loop.
    // such as:
    // y.exit = phi [ y.loop, latch ], [ y.entry, preHeader ]
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
                ++it;
                phi->eraseInst();
            } else {
                ++it;
            }
        }
    }

    // Redirect the unique entry edge to the exit block.
    auto* entryTerm = entryInsts.back();
    entryTerm->replaceAllUsesWith(nullptr);
    entryTerm->eraseInst();

    new BranchInst(exitBB, preHeader);
    return true;
}

// Collapse a redundant repeat loop to one execution.
// Keep one body execution and cut the backedge.
static bool tryCollapseRepeatLoop(Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev) {
    if (!L || !exitBB || !L->head || !L->latch) return false;
    if (L->latches.size() != 1) return false;
    auto* preHeader = L->entryBlock(dt);
    if (!preHeader) return false;

    // Require a compile-time trip count >= 1.
    ExitBranchInfo info;
    int64_t tc = -1;
    bool gotTrip =
        (analyzeExitBranch(L, L->latch, scev, info) && getConstantTripCountFromInfo(info, L, tc) && tc >= 1) ||
        (analyzeExitBranch(L, L->head,  scev, info) && getConstantTripCountFromInfo(info, L, tc) && tc >= 1);
    if (!gotTrip) return false;

    // Reject early returns and impure calls. Keep stores as repeated work.
    {
        std::unordered_map<Function*, bool> purity;
        for (auto* bb : L->blocks)
            for (auto* inst : bb->getInstructions()) {
                if (isa<ReturnInst>(inst)) return false;
                if (auto* call = dyn_cast<CallInst>(inst))
                    if (!isPureFunc(call->getFunction(), purity)) return false;
            }
    }

    // Require each header phi to be a control-only IV, invariant, or dead.
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        if (isBadPhi(phi, L, preHeader)) continue;
        if (isControlOnlyIV(phi, L, preHeader)) continue;
        return false; // a real loop-carried scalar -> not idempotent
    }

    // Require every written base to be reset or written before any read.
    if (!noLoopCarriedMemDep(L, dt, scev)) return false;

    // Keep one iteration and cut the backedge.
    auto& latchInsts = L->latch->getInstructions();
    if (latchInsts.empty()) return false;
    auto* br = dyn_cast<BranchInst>(latchInsts.back());
    if (!br || br->getNumOperands() != 3) return false;
    auto* t = dyn_cast<BasicBlock>(br->getOperand(1));
    auto* f = dyn_cast<BasicBlock>(br->getOperand(2));
    if (!t || !f) return false;
    bool tHead = (t == L->head), fHead = (f == L->head);
    if (tHead == fHead) return false;
    if ((tHead ? f : t) != exitBB) return false;

    // Fold header phis to their entry values.
    std::vector<PhiInst*> phis;
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        phis.push_back(phi);
    }
    for (auto* phi : phis) {
        Value* initVal = nullptr;
        for (int k = 0; k < phi->getNumOperands(); k += 2)
            if (dyn_cast<BasicBlock>(phi->getOperand(k + 1)) == preHeader) {
                initVal = phi->getOperand(k);
                break;
            }
        if (!initVal) return false;
        phi->replaceAllUsesWith(initVal);
    }
    for (auto* phi : phis) phi->eraseInst();

    // Redirect the latch to the loop exit.
    br->replaceAllUsesWith(nullptr);
    br->eraseInst();
    new BranchInst(exitBB, L->latch);
    return true;
}

// Collapse a side-effect-free countdown loop to a guarded last-iteration fast path.
// Only support while cond is NEQ.
// Before:
//   while (iv) {
//       live = readonly(iv);
//       iv = iv - 1;
//   }
//
// After:
//   if (iv > 0) {
//       live = readonly(1);
//   } else {
//       while (iv) { ...original loop... }
//   }
/*
preHeader
├── stepVal > 0 :
│   ├── initVal < stopVal
│   │   ├── (stopVal - initVal) % stepVal == 0
│   │   │   └── fast path: lastIV = stopVal - stepVal
│   │   └── else
│   │       └── slowBB
│   └── else
│       └── slowBB
│
├── stepVal < 0 :
│   ├── initVal > stopVal
│   │   ├── (initVal - stopVal) % (-stepVal) == 0
│   │   │   └── fast path: lastIV = stopVal - stepVal
│   │   └── else
│   │       └── slowBB
│   └── else
│       └── slowBB
│
└── slowBB:
    ├── initVal != stopVal
    │   └── goto loop head
    └── initVal == stopVal
        └── goto exit
*/
static bool tryCollapseDeadLoop(Loop* L, BasicBlock* exitBB, Dominators& dt, SCEV& scev) {
    if (!L || !exitBB || !L->head || !L->latch) return false;
    if (L->latches.size() != 1 || !L->sub.empty()) return false;

    std::function<bool(Function*, std::unordered_map<Function*, bool>&)> 
    isReadOnly = [&](Function* f, std::unordered_map<Function*, bool>& cache) -> bool {
        if (!f || f->getBody()->getBlocks().empty()) return false;
        auto it = cache.find(f);
        if (it != cache.end()) return it->second;
        cache[f] = true;

        for (auto* bb : f->getBody()->getBlocks()) {
            for (auto* inst : bb->getInstructions()) {
                if (isa<StoreInst>(inst)) {
                    cache[f] = false;
                    return false;
                }
                if (isa<ReturnInst>(inst)) continue;
                if (auto* call = dyn_cast<CallInst>(inst)) {
                    if (!isReadOnly(call->getFunction(), cache)) {
                        cache[f] = false;
                        return false;
                    }
                }
            }
        }
        return true;
    };

    std::unordered_map<Function*, bool> cache;
    for (auto* bb : L->blocks) {
        for (auto* inst : bb->getInstructions()) {
            if (isa<ReturnInst>(inst) || isa<StoreInst>(inst)) return false;
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isReadOnly(call->getFunction(), cache)) return false;
        }
    }

    auto* preHeader = L->entryBlock(dt);
    if (!preHeader) return false;

    ExitBranchInfo info;
    if (!analyzeExitBranch(L, L->latch, scev, info)) return false;

    std::map<Value*, Value*> vmap;
    PhiInst* lastIV = nullptr;
    Value* initVal = nullptr;
    Value* stopVal = nullptr;
    int64_t stepVal = 0;
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;

        // Match a counted IV whose latch tests next != stop.
        if (!lastIV && phi->getNumOperands() == 4 && info.continuePred == ICmpInst::NE) {
            Value* fromPreH = nullptr;
            Value* fromLatch = nullptr;
            for (int k = 0; k < phi->getNumOperands(); k += 2) {
                auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
                if (fromBB == preHeader) fromPreH = phi->getOperand(k);
                else if (fromBB && L->has(fromBB)) fromLatch = phi->getOperand(k);
            }

            auto* next = dyn_cast<BinaryInst>(fromLatch);
            auto matchStep = [&](BinaryInst* inst, int64_t& step) {
                if (!inst) return false;
                if (inst->getOpID() == Instruction::Sub) {
                    auto* c = dyn_cast<ConstantInt>(inst->getOperand(1));
                    if (inst->getOperand(0) != phi || !c || c->getValue() == 0)
                        return false;
                    step = -c->getValue();
                    return true;
                }
                if (inst->getOpID() == Instruction::Add) {
                    auto chk = [&](Value* a, Value* b, int64_t& out) {
                        auto* c = dyn_cast<ConstantInt>(b);
                        if (a != phi || !c || c->getValue() == 0) return false;
                        out = c->getValue();
                        return true;
                    };
                    return chk(inst->getOperand(0), inst->getOperand(1), step) ||
                           chk(inst->getOperand(1), inst->getOperand(0), step);
                }
                return false;
            };

            Value* candStop = nullptr;
            auto isNextStop = [&](Value* a, Value* b) {
                if (a != fromLatch || !isLoopInvariantValue(b, L)) return false;
                candStop = b;
                return true;
            };
            bool latchTestsNextStop = isNextStop(info.lhs, info.rhs) ||
                                      isNextStop(info.rhs, info.lhs);

            int64_t matchedStep = 0;
            if (fromPreH && fromLatch && matchStep(next, matchedStep) && latchTestsNextStop) {
                initVal = fromPreH;
                stopVal = candStop;
                stepVal = matchedStep;
                lastIV = phi;
                continue;
            }
        }

        // Here processes non-IV phis.
        if (!isBadPhi(phi, L, preHeader)) return false;
        for (int k = 0; k < phi->getNumOperands(); k += 2) {
            if (dyn_cast<BasicBlock>(phi->getOperand(k + 1)) == preHeader) {
                vmap[phi] = phi->getOperand(k);
                break;
            }
        }
        if (!vmap.count(phi)) return false;
    }
    if (!lastIV || !initVal || !stopVal || stepVal == 0) return false;

    // Checking structure.
    auto& entryInsts = preHeader->getInstructions();
    if (entryInsts.empty()) return false;
    auto* entryBr = dyn_cast<BranchInst>(entryInsts.back());
    if (!entryBr || entryBr->getNumOperands() != 3) return false;
    Value* oldCond = entryBr->getOperand(0);
    auto* oldTrue = dyn_cast<BasicBlock>(entryBr->getOperand(1));
    auto* oldFalse = dyn_cast<BasicBlock>(entryBr->getOperand(2));
    if (!oldTrue || !oldFalse) return false;
    auto* entryCmp = dyn_cast<ICmpInst>(oldCond);
    if (!entryCmp) return false;
    bool trueHead = oldTrue == L->head;
    bool falseHead = oldFalse == L->head;
    if (trueHead == falseHead) return false;
    if ((trueHead ? oldFalse : oldTrue) != exitBB) return false;

    // entryCmp = (initVal != stopVal)
    // br entryCmp, ...
    auto isInitStop = [&](Value* a, Value* b) {
        return a == initVal && sameValue(b, stopVal, scev);
    };
    bool initStop = isInitStop(entryCmp->getOperand(0), entryCmp->getOperand(1)) ||
                    isInitStop(entryCmp->getOperand(1), entryCmp->getOperand(0));
    if (!initStop) return false;
    if ((entryCmp->getPredicate() == ICmpInst::NE && !trueHead) ||
        (entryCmp->getPredicate() == ICmpInst::EQ && !falseHead))
        return false;
    if (entryCmp->getPredicate() != ICmpInst::NE && entryCmp->getPredicate() != ICmpInst::EQ)
        return false;

    BasicBlock* tHead = trueHead ? oldTrue : oldFalse;
    BasicBlock* tExit = trueHead ? oldFalse : oldTrue;

    // Transform:
    //      if (initVal > 0) {
    //          fast path
    //      } else {
    //          slow path:
    //              if (initVal < 0) goto old loop head;
    //              else goto exit
    //      }
    auto* region = preHeader->getParent();
    if (!region) return false;
    
    // fast path
    BasicBlock* lastIVBB = nullptr;
    if (auto* c = dyn_cast<ConstantInt>(stopVal)) {
        vmap[lastIV] = new ConstantInt(c->getValue() - stepVal);
    } else if (stepVal > 0) {
        lastIVBB = new BasicBlock("fastBB", region);
        vmap[lastIV] = new BinaryInst(Instruction::Sub, stopVal, new ConstantInt(stepVal), lastIVBB);
    } else {
        lastIVBB = new BasicBlock("fastBB", region);
        vmap[lastIV] = new BinaryInst(Instruction::Add, stopVal, new ConstantInt(-stepVal), lastIVBB);
    }

    LoopOneIterClone fastClone;
    if (!cloneLoopOneIteration(L, exitBB, region, vmap, fastClone)) return false;
    vmap = fastClone.valueMap;
    if (lastIVBB)
        new BranchInst(fastClone.entry, lastIVBB);

    // slow path
    auto* slowBB = new BasicBlock("slowBB", region);
    auto* slowEntryBB = new BasicBlock("slowEntryBB", region);
    auto* slowCond = new ICmpInst(ICmpInst::NE, initVal, stopVal, slowBB);
    new BranchInst(slowCond, slowEntryBB, tExit, slowBB);
    new BranchInst(tHead, slowEntryBB);

    // Fix header phis.
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        for (int k = 0; k < phi->getNumOperands(); k += 2)
            if (dyn_cast<BasicBlock>(phi->getOperand(k + 1)) == preHeader)
                phi->setOperand(k + 1, slowEntryBB);
    }

    // Fix exit phis.
    // preHeader
    //   ├── fastBB  -> exit
    //   └── slowBB
    //         ├── old loop -> exit
    //         └── exit
    //
    // %ans.exit = phi [ %ans.loop, loop ],
    //                 [ %ans.init, slowBB ],
    //                 [ %ans.fast, cloned-exit-pred ]
    for (auto* inst : exitBB->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;

        std::map<BasicBlock*, Value*> loopVals;
        bool hasEntryIncoming = false;
        for (int k = 0; k < phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            // preHeader:
            //      br ..., loop, exit
            //
            // turns to:
            //
            // preHeader:
            //      br ..., fastBB, slowBB
            if (fromBB == preHeader) {
                phi->setOperand(k + 1, slowBB);
                hasEntryIncoming = true;
            } else if (fromBB && L->has(fromBB)) {
                loopVals[fromBB] = phi->getOperand(k);
            }
        }
        if (!loopVals.empty()) {
            for (auto& [origPred, clonedPred] : fastClone.exitEdges) {
                auto lv = loopVals.find(origPred);
                if (lv == loopVals.end()) return false;
                Value* loopVal = lv->second;
            auto it = vmap.find(loopVal);
            Value* fastVal = (it != vmap.end()) ? it->second : loopVal;
            // If loopVal is inst, which is coming from loop.
            // Then fastVal != loopVal
            if (auto* def = dyn_cast<Instruction>(loopVal))
                if (L->has(def->getParent()) && fastVal == loopVal)
                    return false;

                phi->addIncoming(fastVal, clonedPred);
            }
        } else
            return false;
    }

    auto emitPre = [&](Instruction* inst) -> Instruction* {
        inst->setParent(preHeader);
        entryInsts.insert(std::prev(entryInsts.end()), inst);
        return inst;
    };
    // Determination of generation direction.
    auto* dir = static_cast<ICmpInst*>(emitPre(
        new ICmpInst(stepVal > 0 ? ICmpInst::SLT : ICmpInst::SGT, initVal, stopVal, nullptr)));
    Value* fastCond = dir;
    int64_t absStep = stepVal > 0 ? stepVal : -stepVal;
    // If the step size is not 1, 
    // then need to check whether the distance is divisible by the step.
    if (absStep != 1) {
        // aligned = (diff % absStep == 0)
        // then goto fast path.
        auto* diff = static_cast<BinaryInst*>(emitPre(
            stepVal > 0
                ? new BinaryInst(Instruction::Sub, stopVal, initVal, nullptr)
                : new BinaryInst(Instruction::Sub, initVal, stopVal, nullptr)));
        auto* rem = static_cast<BinaryInst*>(emitPre(
            new BinaryInst(Instruction::Mod, diff, new ConstantInt(absStep), nullptr)));
        auto* aligned = static_cast<ICmpInst*>(emitPre(
            new ICmpInst(ICmpInst::EQ, rem, new ConstantInt(0), nullptr)));
        fastCond = emitPre(new BinaryInst(Instruction::And, dir, aligned, nullptr));
    }
    entryBr->replaceAllUsesWith(nullptr);
    entryBr->eraseInst();
    new BranchInst(fastCond, lastIVBB ? lastIVBB : fastClone.entry, slowBB, preHeader);
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

            // Collapse repeat loops before rejecting loops with subloops.
            if (L->exits.size() == 1 &&
                tryCollapseRepeatLoop(L, L->exits[0], dt, scev))
                return true;

            // while(n) {readOnly} -> if(n) {readOnly when IV = 1} else {slowpath}
            if (L->exits.size() == 1 &&
                tryCollapseDeadLoop(L, L->exits[0], dt, scev))
                return true;

            // Stay conservative while subloops remain.
            if (!L->sub.empty()) return false;

            // Allow multiple exiting blocks, but require one exit block.
            if (L->exits.size() != 1) return false;
            auto* exitBB = L->exits[0];

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
