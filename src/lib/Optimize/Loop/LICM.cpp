#include "Optimize/Loop/LICM.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>

using namespace sysy;

static int LICMHoistPreBBID = 0;

static std::string hoistedPreName(BasicBlock* head) {
    return "pre_" + head->getName() + "_h" + std::to_string(LICMHoistPreBBID++);
}

// Get CFG pred list for targetBB in the same parent region.
static std::vector<BasicBlock*> getActualPredecessors(BasicBlock* target) {
    std::vector<BasicBlock*> preds;
    if (!target || !target->getParent()) return preds;

    std::set<BasicBlock*> uniq;
    for (auto bb : target->getParent()->getBlocks()) {
        if (bb->getInstructions().empty()) continue;
        auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
        if (!br) continue;

        int begin = br->getNumOperands() == 1 ? 0 : 1;
        for (int i = begin; i < br->getNumOperands(); ++i) {
            if (dyn_cast<BasicBlock>(br->getOperand(i)) == target &&
                uniq.insert(bb).second) {
                preds.push_back(bb);
            }
        }
    }
    return preds;
}

static bool valueAvailableAtBypass(Value* v, Loop* inner) {
    auto* def = dyn_cast<Instruction>(v);
    return !def || !inner->has(def->getParent());
}

static bool canRedirectPhiIncoming(BasicBlock* bb, BasicBlock* oldPred,
                                   Loop* inner) {
    for (auto* inst : bb->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            if (phi->getOperand(k + 1) != oldPred) continue;
            if (!valueAvailableAtBypass(phi->getOperand(k), inner))
                return false;
        }
    }
    return true;
}

static void redirectPhiIncoming(BasicBlock* bb, BasicBlock* oldPred,
                                BasicBlock* newPred) {
    for (auto* inst : bb->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        for (int k = 1; k < (int)phi->getNumOperands(); k += 2) {
            if (phi->getOperand(k) == oldPred)
                phi->setOperand(k, newPred);
        }
    }
}

// Follow GEP/phi chains to the root pointer (global, alloca, or arg).
static Value* getBaseObject(Value* v, std::set<Value*>& vis) {
    if (!vis.insert(v).second) return v;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return getBaseObject(gep->getOperand(0), vis);
    if (auto* phi = dyn_cast<PhiInst>(v)) {
        Value* base = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            Value* b = getBaseObject(phi->getOperand(k), vis);
            if (!base) base = b;
            else if (base != b) return v;
        }
        return base ? base : v;
    }
    return v;
}
static Value* getBaseObject(Value* v) {
    std::set<Value*> vis;
    return getBaseObject(v, vis);
}

// True if f contains no stores (transitively). Conservative for external fns.
static bool isReadOnlyFunc(Function* f, std::unordered_map<Function*, bool>& cache) {
    if (!f) return false;
    auto* body = f->getBody();
    if (!body || body->getBlocks().empty()) return false;
    auto it = cache.find(f);
    if (it != cache.end()) return it->second;
    cache[f] = true;
    for (auto bb : body->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (isa<StoreInst>(inst)) { cache[f] = false; return false; }
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isReadOnlyFunc(call->getFunction(), cache)) {
                    cache[f] = false; return false;
                }
        }
    }
    return true;
}

bool LICM::run() {
    bool any = false;
    purityCache.clear();
    readOnlyCache.clear();
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool LICM::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    bool changed = false;

    {
        Dominators dt(f); dt.run();
        LoopInfo li(f, dt);
        SCEV scev(f, li);
        std::function<void(Loop*)> visit = [&](Loop* L) {
            for (auto sub : L->sub) visit(sub);
            changed |= unifyIndVars(L, scev);
            changed |= hoistLoop(L, dt, scev);
            changed |= promoteLoop(L, dt);
        };
        for (auto top : li.tops()) visit(top);
    }

    // Hoist entire outer-invariant inner loops before the outer loop, but only
    // when the CFG/phi rewrites are proven safe for the bypassed inner preheader.
    bool hoisted;
    do {
        hoisted = false;
        Dominators dt(f); dt.run();
        LoopInfo li(f, dt);
        for (auto outer : li.tops()) {
            if (tryHoistSubloop(outer)) {
                changed = hoisted = true;
                break;
            }
        }
    } while (hoisted);

    return changed;
}

bool LICM::isFullyOuterInvariant(Loop* outer, Loop* inner) {
    std::set<BasicBlock*> outerBBs(outer->blocks.begin(), outer->blocks.end());
    std::set<BasicBlock*> innerBBs(inner->blocks.begin(), inner->blocks.end());

    // Collect write-bases from outer-only blocks (for inner load alias check).
    std::set<Value*> owb;
    for (auto bb : outerBBs) {
        if (innerBBs.count(bb)) continue;
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst))
                owb.insert(getBaseObject(st->getOperand(1)));
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isPureFunc(call->getFunction(), purityCache))
                    return false;
        }
    }

    for (auto bb : inner->blocks) {
        for (auto inst : bb->getInstructions()) {
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isPureFunc(call->getFunction(), purityCache))
                    return false;
            // No operand may be defined in an outer-only block.
            for (int k = 0; k < (int)inst->getNumOperands(); k++) {
                auto* def = dyn_cast<Instruction>(inst->getOperand(k));
                if (def && outerBBs.count(def->getParent()) && !innerBBs.count(def->getParent()))
                    return false;
            }
            if (auto* ld = dyn_cast<LoadInst>(inst))
                if (owb.count(getBaseObject(ld->getOperand(0)))) return false;
            if (auto* st = dyn_cast<StoreInst>(inst))
                if (owb.count(getBaseObject(st->getOperand(1)))) return false;
        }
    }
    return true;
}

bool LICM::tryHoistSubloop(Loop* outer) {
    for (auto inner : outer->sub)
        if (tryHoistSubloop(inner)) return true;

    // Fold single-operand phis whose value is defined before the outer loop.
    {
        std::set<BasicBlock*> outerSet(outer->blocks.begin(), outer->blocks.end());
        bool folded;
        do {
            folded = false;
            for (auto bb : outer->blocks) {
                auto& ins = bb->getInstructions();
                for (auto it = ins.begin(); it != ins.end(); ) {
                    auto* phi = dyn_cast<PhiInst>(*it);
                    if (!phi || phi->getNumOperands() != 2) { ++it; continue; }
                    // fix: get new preds list, not rely on the old CFG.
                    auto preds = getActualPredecessors(bb);
                    auto* inBB = dyn_cast<BasicBlock>(phi->getOperand(1));
                    if (preds.size() != 1 || !inBB || preds[0] != inBB) {
                        ++it;
                        continue;
                    }
                    Value* val = phi->getOperand(0);
                    auto* def = dyn_cast<Instruction>(val);
                    if (def && outerSet.count(def->getParent())) { ++it; continue; }
                    phi->replaceAllUsesWith(val);
                    it = ins.erase(it);
                    folded = true;
                }
            }
        } while (folded);
    }

    for (auto inner : outer->sub) {
        if (!inner->pre || !outer->pre || inner->latches.empty()) continue;

        // Reject multi-latch or multi-exit inner loops.
        if (inner->latches.size() > 1 || inner->exits.size() > 1) continue;
        if (inner->exiting.size() != 1 || inner->exits.size() != 1) continue;

        if (!isFullyOuterInvariant(outer, inner)) continue;

        auto* ibr = dyn_cast<BranchInst>(inner->head->getInstructions().back());
        if (!ibr || ibr->getNumOperands() != 3) continue;
        BasicBlock* t1 = cast<BasicBlock>(ibr->getOperand(1));
        BasicBlock* t2 = cast<BasicBlock>(ibr->getOperand(2));
        bool t1In = inner->has(t1);
        bool t2In = inner->has(t2);
        if (t1In == t2In) continue;
        if (inner->exiting[0] != inner->head) continue;
        BasicBlock* iexit = t1In ? t2 : t1;
        if (iexit != inner->exits[0]) continue;

        auto* opbr = dyn_cast<BranchInst>(outer->pre->getInstructions().back());
        if (!opbr || opbr->getNumOperands() != 1) continue;
        if (cast<BasicBlock>(opbr->getOperand(0)) != outer->head) continue;

        auto* ipbr = dyn_cast<BranchInst>(inner->pre->getInstructions().back());
        if (!ipbr || ipbr->getNumOperands() != 1) continue;
        if (cast<BasicBlock>(ipbr->getOperand(0)) != inner->head) continue;

        if (!canRedirectPhiIncoming(iexit, inner->head, inner)) continue;

        // CFG transformation: hoist inner loop before outer loop.
        Region* region = outer->pre->getParent();
        auto* npre = new BasicBlock(hoistedPreName(outer->head), region);
        new BranchInst(outer->head, npre);
        auto& blist = region->getBlocks();
        blist.splice(std::find(blist.begin(), blist.end(), outer->head),
                     blist, std::prev(blist.end()));

        opbr->replaceSuccessor(outer->head, inner->head); // outer_pre -> inner_head
        ibr->replaceSuccessor(iexit, npre);                // inner exits -> new outer_pre
        ipbr->replaceSuccessor(inner->head, iexit);        // inner_pre bypasses -> inner_exit

        //  outer->pre ──→ outer->head ──→ ... ──→ inner->pre ──→ inner->head       
        //                  ↑                                       ↓ exit       
        //                  └──────────────────────────────────   iexit      
        //
        // becomes:
        //
        // outer->pre ──→ inner->head ──→ (inner loop)                      
        //                 ↓ exit                                               
        //                npre ──→ outer->head ──→ ... ──→ inner->pre ──→ iexit 
        //                            ↑                                       ↓ 
        //                            └───────────────────────────────────────┘ 
        redirectPhiIncoming(inner->head, inner->pre, outer->pre);
        redirectPhiIncoming(iexit, inner->head, inner->pre);
        redirectPhiIncoming(outer->head, outer->pre, npre);
        return true;
    }
    return false;
}

// Domtree DFS LICM. hoistable flag: starts true; Load/Branch sets it false,
// preventing stores in the same domtree subtree from being hoisted.
bool LICM::hoistLoop(Loop* L, Dominators& dt, SCEV& scev) {
    auto* preBlock = L->entryBlock(dt);
    if (!preBlock || !L->latch) return false;
    {
        auto* lbr = dyn_cast<BranchInst>(L->latch->getInstructions().back());
        if (!lbr || lbr->getNumOperands() != 3) return false;
    }

    std::vector<StoreInst*> stores;
    bool impure = false, hasMW = false;
    for (auto bb : L->blocks) {
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst)) stores.push_back(st);
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (!isPureFunc(call->getFunction(), purityCache)) {
                    impure = true;
                    if (!isReadOnlyFunc(call->getFunction(), readOnlyCache))
                        hasMW = true;
                }
            }
        }
    }

    std::map<BasicBlock*, std::vector<BasicBlock*>> domCh;
    for (auto bb : L->blocks) domCh[bb] = {};
    for (auto bb : L->blocks) {
        if (bb == L->head) continue;
        auto* idom = dt.getIDom(bb);
        if (idom && domCh.count(idom)) domCh[idom].push_back(bb);
    }

    std::set<Instruction*> inv;
    auto outside = [&](Value* v) -> bool {
        if (!isa<Instruction>(v)) return true;
        auto* i = cast<Instruction>(v);
        return !L->has(i->getParent()) || inv.count(i);
    };

    auto& pre = preBlock->getInstructions();
    auto ins_pt = std::prev(pre.end());
    bool any = false;

    std::function<void(BasicBlock*, bool)> visit = [&](BasicBlock* bb, bool hoistable) {
        std::vector<Instruction*> toHoist;

        // Only hoist from latch-dominating blocks (unconditional every iteration).
        // Speculative hoist from conditional paths extends live ranges for free.
        bool execsEveryIter = dt.dominates(bb, L->latch);

        for (auto inst : bb->getInstructions()) {
            auto op = inst->getOpID();

            if (op == Instruction::Load || op == Instruction::Br)
                hoistable = false;

            if (op == Instruction::Br  || op == Instruction::Ret ||
                op == Instruction::Phi || op == Instruction::Alloca)
                continue;

            if (op == Instruction::Call) {
                auto* call = cast<CallInst>(inst);
                if (!isPureFunc(call->getFunction(), purityCache)) continue;
                bool ok = true;
                for (int i = 1; i < (int)inst->getNumOperands(); i++)
                    if (!outside(inst->getOperand(i))) { ok = false; break; }
                if (ok) { inv.insert(inst); toHoist.push_back(inst); }
                continue;
            }

            if (op == Instruction::Store) {
                if (!hoistable || impure) continue;
                if (!outside(inst->getOperand(0)) || !outside(inst->getOperand(1))) continue;
                inv.insert(inst); toHoist.push_back(inst);
                continue;
            }

            if (op == Instruction::Load) {
                if (!execsEveryIter || hasMW || !outside(inst->getOperand(0))) continue;
                Value* lb = getBaseObject(inst->getOperand(0));
                SE* lse = scev.get(inst->getOperand(0));
                bool alias = false;
                for (auto* st : stores) {
                    if (inv.count(st)) continue;
                    if (getBaseObject(st->getOperand(1)) != lb) continue;
                    if (!scev.distinct(lse, scev.get(st->getOperand(1)))) { alias = true; break; }
                }
                if (!alias) { inv.insert(inst); toHoist.push_back(inst); }
                continue;
            }

            bool ok = true;
            for (int i = 0; i < (int)inst->getNumOperands(); i++)
                if (!outside(inst->getOperand(i))) { ok = false; break; }
            if (ok && execsEveryIter) { inv.insert(inst); toHoist.push_back(inst); }
        }

        for (auto* inst : toHoist) {
            bb->getInstructions().remove(inst);
            inst->setParent(preBlock);
            pre.insert(ins_pt, inst);
        }
        any |= !toHoist.empty();

        for (auto* child : domCh[bb])
            visit(child, hoistable);
    };

    visit(L->head, true);
    return any;
}

// Replace duplicate induction variable phis(same SCEV expression), 
// at the loop header with a single canonical phi.
bool LICM::unifyIndVars(Loop* L, SCEV& scev) {
    if (!L->head) return false;

    // Collect all phi nodes at the loop header.
    std::vector<PhiInst*> phis;
    for (auto inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        phis.push_back(phi);
    }
    if (phis.size() < 2) return false;

    // Build SCEV for each phi, skipping Unknown (no pattern match).
    std::vector<SE*> ses;
    ses.reserve(phis.size());
    for (auto* phi : phis)
        ses.push_back(scev.get(phi));

    bool any = false;
    // For each pair (i, j) with i < j, if equal SCEVs → replace j with i.
    std::vector<bool> dead(phis.size(), false);
    for (size_t i = 0; i < phis.size(); i++) {
        if (dead[i]) continue;
        if (isa<SEUnknown>(ses[i])) continue;
        for (size_t j = i + 1; j < phis.size(); j++) {
            if (dead[j]) continue;
            if (phis[i]->getType() != phis[j]->getType()) continue;
            if (!scev.equal(ses[i], ses[j])) continue;
            // phis[j] has same evolution as phis[i]: replace all uses.
            phis[j]->replaceAllUsesWith(phis[i]);
            dead[j] = true;
            any = true;
        }
    }

    // Remove dead phis from the header (back-to-front to preserve iterators).
    for (int k = (int)phis.size() - 1; k >= 0; k--) {
        if (!dead[k]) continue;
        L->head->getInstructions().remove(phis[k]);
    }
    return any;
}

bool LICM::promoteLoop(Loop* L, Dominators& dt) {
    auto* preBlock = L->entryBlock(dt);
    if (!preBlock || !L->latch || !L->head) return false;

    auto* pbr = dyn_cast<BranchInst>(preBlock->getInstructions().back());
    if (!pbr || pbr->getNumOperands() != 3) return false;
    auto* lbr = dyn_cast<BranchInst>(L->latch->getInstructions().back());
    if (!lbr || lbr->getNumOperands() != 3) return false;

    std::set<BasicBlock*> lbbs(L->blocks.begin(), L->blocks.end());

    // Identify the single exit block (pre and latch must exit to the same block).
    auto exitOf = [&](BranchInst* br) -> BasicBlock* {
        auto* t1 = cast<BasicBlock>(br->getOperand(1));
        auto* t2 = cast<BasicBlock>(br->getOperand(2));
        return lbbs.count(t1) ? t2 : t1;
    };
    BasicBlock* exitBB = exitOf(pbr);
    if (exitBB != exitOf(lbr)) return false;

    // Single-exit check: no loop block may branch to outside the loop except to exitBB.
    // Handles break/return inside the loop body (would leave store un-executed).
    for (auto bb : L->blocks) {
        auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
        if (!br) continue;
        int start = (br->getNumOperands() == 1) ? 0 : 1;
        for (int k = start; k < (int)br->getNumOperands(); k++) {
            auto* succ = dyn_cast<BasicBlock>(br->getOperand(k));
            if (succ && !lbbs.count(succ) && succ != exitBB) return false;
        }
    }

    // Check if any call with side effects could alias the promoted address.
    for (auto bb : L->blocks)
        for (auto inst : bb->getInstructions())
            if (auto* c = dyn_cast<CallInst>(inst))
                if (!isPureFunc(c->getFunction(), purityCache)) return false;

    auto outside = [&](Value* v) -> bool {
        if (auto* i = dyn_cast<Instruction>(v)) return !L->has(i->getParent());
        return true;
    };

    // Collect slots
    struct Slot { std::vector<LoadInst*> lds; std::vector<StoreInst*> sts; };
    std::map<Value*, Slot> slots;
    for (auto bb : L->blocks) {
        for (auto inst : bb->getInstructions()) {
            if (auto* ld = dyn_cast<LoadInst>(inst))
                if (outside(ld->getOperand(0)))
                    slots[ld->getOperand(0)].lds.push_back(ld);
            if (auto* st = dyn_cast<StoreInst>(inst))
                if (outside(st->getOperand(1)))
                    slots[st->getOperand(1)].sts.push_back(st);
        }
    }

    bool any = false;
    for (auto& [addr, slot] : slots) {
        if (slot.lds.empty() || slot.sts.size() != 1) continue;

        StoreInst* st = slot.sts[0];
        Value* sval = st->getOperand(0);

        // Store block must dominate latch so sval is available at latch.
        BasicBlock* stBB = st->getParent();
        if (stBB != L->latch && !dt.dominates(stBB, L->latch)) continue;

        // Alias check: no other store or untracked load to the same base object.
        Value* base = getBaseObject(addr);
        bool alias = false;
        for (auto bb : L->blocks) {
            for (auto inst : bb->getInstructions()) {
                if (auto* s = dyn_cast<StoreInst>(inst)) {
                    if (s == st) continue;
                    if (getBaseObject(s->getOperand(1)) == base) { alias = true; break; }
                }
                if (auto* l = dyn_cast<LoadInst>(inst)) {
                    if (std::find(slot.lds.begin(), slot.lds.end(), l) != slot.lds.end()) continue;
                    if (getBaseObject(l->getOperand(0)) == base) { alias = true; break; }
                }
            }
            if (alias) break;
        }
        if (alias) continue;

        Type* ty = slot.lds[0]->getType();

        // Insert preload in preheader before its branch.
        auto* preload = new LoadInst(addr, nullptr);
        preload->setParent(preBlock);
        { auto& ins = preBlock->getInstructions(); ins.insert(std::prev(ins.end()), preload); }

        // Insert loop header phi: phi(preload[pre], sval[latch]).
        auto* hphi = new PhiInst(ty, nullptr);
        hphi->setParent(L->head);
        hphi->addIncoming(preload, preBlock);
        hphi->addIncoming(sval, L->latch);
        L->head->getInstructions().push_front(hphi);

        // Replace all loop loads with hphi; remove the store.
        for (auto* ld : slot.lds) {
            ld->replaceAllUsesWith(hphi);
            ld->getParent()->getInstructions().remove(ld);
        }
        st->getParent()->getInstructions().remove(st);

        auto* ephi = new PhiInst(ty, nullptr);
        ephi->setParent(exitBB);
        ephi->addIncoming(preload, preBlock);
        ephi->addIncoming(sval, L->latch);
        {
            auto& ins = exitBB->getInstructions();
            auto it = ins.begin();
            while (it != ins.end() && isa<PhiInst>(*it)) ++it;
            ins.insert(it, ephi);
        }

        auto* estore = new StoreInst(ephi, addr, nullptr);
        estore->setParent(exitBB);
        {
            auto& ins = exitBB->getInstructions();
            auto it = ins.begin();
            while (it != ins.end() && isa<PhiInst>(*it)) ++it;
            ins.insert(it, estore);
        }

        any = true;
    }
    return any;
}
