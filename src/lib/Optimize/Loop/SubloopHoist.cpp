#include "Optimize/Loop/SubloopHoist.h"
#include "Optimize/Loop/LoopUtils/LoopAliasUtils.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <set>
#include <string>

using namespace sysy;

static int SubloopHoistPreBBID = 0;

static std::string hoistedPreName(BasicBlock* head) {
    return "pre_" + head->getName() + "_h" + std::to_string(SubloopHoistPreBBID++);
}

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
                uniq.insert(bb).second)
                preds.push_back(bb);
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

// ── Pass implementation ───────────────────────────────────────────────────────

bool SubloopHoist::run() {
    bool any = false;
    purityCache.clear();
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool SubloopHoist::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    bool changed = false;

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

bool SubloopHoist::isFullyOuterInvariant(Loop* outer, Loop* inner) {
    std::set<BasicBlock*> outerBBs(outer->blocks.begin(), outer->blocks.end());
    std::set<BasicBlock*> innerBBs(inner->blocks.begin(), inner->blocks.end());

    std::set<BasicBlock*> restBBs;
    for (auto bb : outerBBs)
        if (!innerBBs.count(bb)) restBBs.insert(bb);

    for (auto bb : restBBs)
        for (auto inst : bb->getInstructions())
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isPureFunc(call->getFunction(), purityCache))
                    return false;

    std::set<Value*> owb, orb;
    for (auto bb : restBBs) {
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst))
                owb.insert(getLoopBaseObject(st->getOperand(1)));
            if (auto* ld = dyn_cast<LoadInst>(inst))
                orb.insert(getLoopBaseObject(ld->getOperand(0)));
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
                if (def && outerBBs.count(def->getParent()) &&
                    !innerBBs.count(def->getParent()))
                    return false;
            }
            // Inner load aliases with outer write -> not invariant.
            if (auto* ld = dyn_cast<LoadInst>(inst)) {
                std::set<Value*> vis, ldBases;
                collectAllBases(ld->getOperand(0), vis, ldBases);
                for (auto* b : ldBases)
                    if (owb.count(b)) return false;
            }
            // Inner store aliases with outer read or write -> not invariant.
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                std::set<Value*> vis, stBases;
                collectAllBases(st->getOperand(1), vis, stBases);
                for (auto* b : stBases)
                    if (owb.count(b) || orb.count(b)) return false;
            }
        }
    }
    return true;
}

bool SubloopHoist::tryHoistSubloop(Loop* outer) {
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
                    auto preds = getActualPredecessors(bb);
                    auto* inBB = dyn_cast<BasicBlock>(phi->getOperand(1));
                    if (preds.size() != 1 || !inBB || preds[0] != inBB) {
                        ++it; continue;
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
        ibr->replaceSuccessor(iexit, npre); // inner exits -> new outer_pre
        ipbr->replaceSuccessor(inner->head, iexit); // inner_pre bypasses -> inner_exit

        //  outer->pre ──→ outer->head ──→ ... ──→ inner->pre ──→ inner->head
        //                  ↑                                       ↓ exit
        //                  └────────────────────────────────────  iexit
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
