#include "rv/RegAlloc.h"
#include <algorithm>
#include <cassert>
#include <vector>

namespace sysy {
namespace rv {

static void insertBefore(MCBlock* blk, RvOp* pos, RvOp* op) {
    if (pos) blk->insertBefore(pos, op);
    else blk->append(op);
}

void RegAlloc::emitLS(MCBlock* blk, RvOp* pos,
                        bool isLoad, bool isFP, bool isPtr,
                        Reg reg, Reg base, int offset,
                        Reg scratch) {
    auto emit = [&](Reg b, int off) {
        RvOp* op;
        if (isLoad) {
            if (isPtr) op = new LdOp (MCOperand(reg), MCOperand(b), off);
            else if (isFP) op = new FLwOp(MCOperand(reg), MCOperand(b), off);
            else op = new LwOp (MCOperand(reg), MCOperand(b), off);
        } else {
            if (isPtr) op = new SdOp (MCOperand(reg), MCOperand(b), off);
            else if (isFP) op = new FSwOp(MCOperand(reg), MCOperand(b), off);
            else op = new SwOp (MCOperand(reg), MCOperand(b), off);
        }
        insertBefore(blk, pos, op);
    };

    if (offset >= -2048 && offset <= 2047) {
        emit(base, offset);
        return;
    }

    // Large offset
    insertBefore(blk, pos, new LiOp (MCOperand(scratch), offset));
    insertBefore(blk, pos, new AddOp(MCOperand(scratch), MCOperand(base), MCOperand(scratch)));
    emit(scratch, 0);
}

// Using in emitPrologueEpilogue, so t0 is free.
void RegAlloc::emitAddiSP(MCBlock* blk, RvOp* pos, int delta) {
    if (delta >= -2048 && delta <= 2047) {
        insertBefore(blk, pos, new AddiOp(MCOperand(Reg::sp), MCOperand(Reg::sp), delta));
        return;
    }
    // Large delta: li t0, delta; add sp, sp, t0
    insertBefore(blk, pos, new LiOp (MCOperand(Reg::t0), delta));
    insertBefore(blk, pos, new AddOp(MCOperand(Reg::sp), MCOperand(Reg::sp), MCOperand(Reg::t0)));
}

void RegAlloc::preColor(MCFunction* func) {
    int intIdx = 0, fpIdx = 0;
    for (size_t i = 0; i < func->args.size(); ++i) {
        VReg v = func->args[i];
        bool isFloat = func->argIsFloat[i];
        Reg phys;
        if (isFloat) phys = fargRegs[fpIdx++];
        else phys = argRegs[intIdx++];

        if (liveThroughCall.count(v)) {
            argIncomingReg[v] = phys;
            priority[v] = 100;
        } else {
            assignment[v] = phys;
            priority[v] = 100;
        }
    }
}

static void addInterf(std::unordered_map<VReg, std::unordered_set<VReg>>& g, VReg a, VReg b) {
    g[a].insert(b);
    g[b].insert(a);
}

// When encountering a DEFOp, connect the VReg to all the current live VReg interference edges
void RegAlloc::buildInterference(MCFunction* func) {
    std::vector<MCOperand*> usesBuf;

    for (auto& blkPtr : func->blocks) {
        MCBlock* blk = blkPtr.get();

        // Start with liveOut, walk backwards.
        std::unordered_set<VReg> live = blk->liveOut;

        for (RvOp* op = blk->tail; op; op = op->prev) {
            if (op->opcode == RvOp::CallOp) {
                for (VReg v : live) liveThroughCall.insert(v);
            }

            MCOperand* defOp = op->getDef();
            if (defOp && defOp->isVReg()) {
                VReg d = defOp->getVReg();
                auto* di = func->getVRegInfo(d);
                for (VReg v : live) {
                    if (v == d) continue;
                    auto* vi = func->getVRegInfo(v);
                    bool sameType = di && vi && (di->isFloat == vi->isFloat);
                    if (sameType) addInterf(interf, d, v);
                    else addInterf(spillInterf, d, v);
                }
                live.erase(d);
            } else if (defOp && defOp->isPReg()) {
                // Every live VReg cannot be assigned this physical reg.
                Reg p = defOp->getPReg();
                if (p != Reg::zero && p != Reg::sp) {
                    for (VReg v : live) {
                        pregInterf[v].insert(p);
                    }
                }
            }

            // Handle uses.
            usesBuf.clear();
            op->collectUses(usesBuf);
            for (MCOperand* u : usesBuf) {
                if (u->isVReg()) live.insert(u->getVReg());
            }
            // Handle CallOp stackArgs as implicit uses.
            if (op->opcode == RvOp::CallOp) {
                auto* call = static_cast<CallOp*>(op);
                for (auto& sa : call->stackArgs) {
                    if (sa.src.isVReg()) live.insert(sa.src.getVReg());
                }
            }
        }
    }

    // Handle blocks[0] livein.
    // The parameters interfere with each other.
    if (!func->blocks.empty()) {
        std::vector<VReg> entryLive(func->blocks[0]->liveIn.begin(),
                                    func->blocks[0]->liveIn.end());
        for (size_t i = 0; i < entryLive.size(); ++i) {
            for (size_t j = i + 1; j < entryLive.size(); ++j) {
                VReg a = entryLive[i], b = entryLive[j];
                auto* ai = func->getVRegInfo(a);
                auto* bi = func->getVRegInfo(b);
                if (!ai || !bi) continue;
                if (ai->isFloat == bi->isFloat) addInterf(interf, a, b);
                else                            addInterf(spillInterf, a, b);
            }
        }
    }
}

void RegAlloc::colorGraph(MCFunction* func) {
    bool isLeaf = func->isLeaf;

    // Collect all VRegs with valid info.
    std::vector<VReg> vregs;
    for (size_t i = 1; i < func->vregInfo.size(); ++i) {
        if (func->vregInfo[i].vreg == static_cast<VReg>(i))
            vregs.push_back(static_cast<VReg>(i));
    }

    std::sort(vregs.begin(), vregs.end(), [&](VReg a, VReg b) {
        int pa = priority.count(a) ? priority[a] : 0;
        int pb = priority.count(b) ? priority[b] : 0;
        if (pa != pb) return pa > pb;
        return interf[a].size() > interf[b].size();
    });

    for (VReg v : vregs) {
        if (assignment.count(v)) continue; // pre-colored

        auto* vi = func->getVRegInfo(v);
        if (!vi) continue;

        bool isFloat = vi->isFloat;

        // Build v bad-set from already-assigned interfering VRegs.
        std::unordered_set<Reg> bad;
        for (VReg u : interf[v]) {
            if (assignment.count(u)) {
                Reg r = assignment[u];
                if (r != Reg::sp && r != Reg::zero) bad.insert(r);
            }
        }

        // PReg defs that were live at the same point as v.
        if (pregInterf.count(v)) {
            for (Reg r : pregInterf[v]) bad.insert(r);
        }

        // VRegs live through calls cannot use caller-saved registers.
        if (liveThroughCall.count(v)) {
            for (Reg r : callerSaved) bad.insert(r);
        }

        // Reserve spill registers for RegAlloc's own use.
        bad.insert(spillReg); 
        bad.insert(spillReg2);
        bad.insert(fspillReg);
        bad.insert(fspillReg2);

        const Reg* order = getAllocOrder(isFloat, isLeaf);
        int cnt = getAllocOrderCount(isFloat, isLeaf);

        bool found = false;
        for (int i = 0; i < cnt; ++i) {
            if (!bad.count(order[i])) {
                assignment[v] = order[i];
                found = true;
                break;
            }
        }
        if (!found) {
            // Spill: recorded in spillLocal later.
            spillLocal[v] = -1;
        }
    }
}

void RegAlloc::assignSpillSlots(MCFunction* func) {
    if (spillLocal.empty()) return;

    // Collect spilled VRegs.
    std::vector<VReg> spilled;
    for (auto& [v, _] : spillLocal) spilled.push_back(v);

    // If int spills <= 2 and float spills <= 2, cover entirely with dedicated spill registers.
    {
        int intCnt = 0, fpCnt = 0;
        for (VReg v : spilled) {
            auto* vi = func->getVRegInfo(v);
            if (vi && vi->isFloat) ++fpCnt; 
            else ++intCnt;
        }
        if (intCnt <= 2 && fpCnt <= 2) {
            int intIdx = 0, fpIdx = 0;
            for (VReg v : spilled) {
                auto* vi = func->getVRegInfo(v);
                if (vi && vi->isFloat)
                    assignment[v] = (fpIdx++ == 0) ? fspillReg : fspillReg2;
                else
                    assignment[v] = (intIdx++ == 0) ? spillReg : spillReg2;
            }
            spillLocal.clear();
            return;
        }
    }

    int currentOff = 0;
    for (VReg v : spilled) {
        auto* vi = func->getVRegInfo(v);
        int sz = vi->isPtr ? 8 : 4;

        std::vector<std::pair<int,int>> conflicts; // (start, size)
        auto addConflict = [&](VReg u) {
            if (!spillLocal.count(u) || spillLocal[u] < 0) return;
            auto* vu = func->getVRegInfo(u);
            if (!vu) return;
            conflicts.emplace_back(spillLocal[u], vu->isPtr ? 8 : 4);
        };

        for (VReg u : interf[v]) addConflict(u);
        for (VReg u : spillInterf[v]) addConflict(u);

        int slot = 0;
        bool fits = false;
        while (!fits) {
            slot = (slot + sz - 1) & ~(sz - 1); // align
            fits = true;
            for (auto& [start, szu] : conflicts) {
                if (slot < start + szu && start < slot + sz) { // ranges overlap
                    slot = start + szu; // jump past this neighbor
                    fits = false;
                    break;
                }
            }
        }
        spillLocal[v] = slot;
        if (slot + sz > currentOff) currentOff = slot + sz;
    }
}

void RegAlloc::rewriteOperands(MCFunction* func, int spillBase, int allocaBase) {
    std::vector<MCOperand*> usesBuf;

    for (auto& blkPtr : func->blocks) {
        MCBlock* blk = blkPtr.get();

        // Snapshot the instruction list to avoid iterator invalidation.
        std::vector<RvOp*> ops;
        for (RvOp* op = blk->head; op; op = op->next) ops.push_back(op);

        for (RvOp* op : ops) {
            if (op->opcode == RvOp::CallOp) {
                auto* call = static_cast<CallOp*>(op);
                for (auto& sa : call->stackArgs) {
                    Reg srcReg;
                    bool isPtr = false;

                    if (sa.src.isPReg()) {
                        // Already a physical reg (e.g. Reg::zero for ConstantZero args).
                        srcReg = sa.src.getPReg();
                    } else if (sa.src.isVReg()) {
                        VReg v = sa.src.getVReg();
                        auto* vi = func->getVRegInfo(v);
                        if (!vi) continue;
                        isPtr = vi->isPtr;
                        if (spillLocal.count(v)) {
                            srcReg = vi->isFloat ? fspillReg : spillReg;
                            int off = spillBase + spillLocal[v];
                            emitLS(blk, op, /*load=*/true, vi->isFloat, isPtr, 
                                srcReg, Reg::sp, off);
                        } else {
                            srcReg = assignment.count(v) ? assignment[v] : Reg::zero;
                        }
                        sa.src = MCOperand(srcReg);
                    } else {
                        continue; // empty/invalid operand
                    }

                    int stackOff = sa.slotIdx * 8;
                    emitLS(blk, op, /*load=*/false, sa.isFloat, isPtr,
                        srcReg, Reg::sp, stackOff);
                }
                continue;
            }

            // Handle uses.
            usesBuf.clear();
            op->collectUses(usesBuf);

            int intSpillIdx = 0, fpSpillIdx = 0;

            for (MCOperand* use : usesBuf) {
                if (!use->isVReg()) continue;
                VReg v = use->getVReg();
                auto* vi = func->getVRegInfo(v);
                if (!vi) continue;

                if (spillLocal.count(v)) {
                    bool fp = vi->isFloat;
                    Reg sr = fp ? (fpSpillIdx++ ? fspillReg2 : fspillReg)
                                : (intSpillIdx++ ? spillReg2 : spillReg);

                    int off = spillBase + spillLocal[v];
                    Reg scratch = fp ? (intSpillIdx == 0 ? spillReg : spillReg2) : sr;
                    emitLS(blk, op, /*load=*/true, fp, vi->isPtr, sr, Reg::sp, off, scratch);
                    *use = MCOperand(sr);
                } else {
                    *use = MCOperand(assignment.count(v) ? assignment[v] : Reg::zero);
                }
            }

            // Handle def.
            MCOperand* def = op->getDef();
            if (def && def->isVReg()) {
                VReg v = def->getVReg();
                auto* vi = func->getVRegInfo(v);
                if (!vi) { *def = MCOperand(Reg::zero); continue; }

                if (spillLocal.count(v)) {
                    bool fp = vi->isFloat;
                    Reg sr = fp ? fspillReg : spillReg;
                    *def = MCOperand(sr);
                    int off = spillBase + spillLocal[v];
                    Reg scratch = fp ? spillReg : spillReg2;
                    emitLS(blk, op->next, /*load=*/false, fp, vi->isPtr, sr, Reg::sp, off, scratch);
                } else {
                    *def = MCOperand(assignment.count(v) ? assignment[v] : Reg::zero);
                }
            }
        }
    }

    for (auto& [v, addiOp] : func->allocaAddrInsts) {
        auto* vi = func->getVRegInfo(v);
        if (!vi) continue;
        int localOff = func->allocaLocalOffset.count(v) ? func->allocaLocalOffset[v] : 0;
        int finalOff = allocaBase + localOff;

        if (auto* ai = dynamic_cast<AddiOp*>(addiOp)) {
            if (finalOff >= -2048 && finalOff <= 2047) {
                ai->imm = finalOff;
            } else {
                MCBlock* parent = ai->parent;
                Reg rd = ai->rd.isPReg() ? ai->rd.getPReg() : Reg::t0;
                RvOp* liOp  = new LiOp (MCOperand(rd), finalOff);
                RvOp* addOp = new AddOp(MCOperand(rd), MCOperand(Reg::sp), MCOperand(rd));
                parent->insertBefore(ai, liOp);
                parent->insertBefore(ai, addOp);
                parent->erase(ai);
            }
        }
    }

    // delete mv pReg, pReg / fmv.s fReg, fReg
    for (auto& blkPtr : func->blocks) {
        MCBlock* blk = blkPtr.get();
        std::vector<RvOp*> ops;
        for (RvOp* op = blk->head; op; op = op->next) ops.push_back(op);
        for (RvOp* op : ops) {
            if (op->opcode == RvOp::MvOp) {
                auto* mv = static_cast<MvOp*>(op);
                if (mv->rd.isPReg() && mv->rs.isPReg() && mv->rd.getPReg() == mv->rs.getPReg())
                    blk->erase(op);
            } else if (op->opcode == RvOp::FMvSOp) {
                auto* fmv = static_cast<FMvSOp*>(op);
                if (fmv->rd.isPReg() && fmv->rs.isPReg() && fmv->rd.getPReg() == fmv->rs.getPReg())
                    blk->erase(op);
            }
        }
    }
}

/*   
    ├─────────────────┤ <- sp                                                                                                                                            
    │ outgoing args   │ 
    ├─────────────────┤
    │ spill slots     │
    ├─────────────────┤
    │ alloca area     │
    ├─────────────────┤
    │ callee-saved+ra │ 
    ├─────────────────┤ <- old sp
*/

void RegAlloc::emitPrologueEpilogue(MCFunction* func) {
    // Find callee-saved physical registers actually used
    std::vector<Reg> usedCalleeSaved;
    {
        std::unordered_set<Reg> seen;
        // This includes spillReg/spillReg2/fspillReg/fspillReg2,
        // when they were assigned to VRegs by the spill optimization.
        for (auto& [v, r] : assignment) {
            if (!seen.count(r) && calleeSaved.count(r)) {
                usedCalleeSaved.push_back(r);
                seen.insert(r);
            }
        }

        if (!spillLocal.empty()) {
            for (Reg r : {spillReg, spillReg2, fspillReg, fspillReg2}) {
                if (!seen.count(r)) { usedCalleeSaved.push_back(r); seen.insert(r); }
            }
        }
    }

    // Stack Arguments Size.
    // All CallOp share the same stack arguments size, so take the max.
    int maxOutBytes = 0;
    func->forEachInst([&](RvOp* op) {
        if (op->opcode == RvOp::CallOp) {
            auto* call = static_cast<CallOp*>(op);
            if (!call->stackArgs.empty()) {
                int last = call->stackArgs.back().slotIdx + 1;
                maxOutBytes = std::max(maxOutBytes, last * 8);
            }
        }
    });
    maxOutBytes = (maxOutBytes + 7) & ~7;

    // Spill Area Size.
    int spillAreaSize = 0;
    for (auto& [v, local] : spillLocal) {
        auto* vi = func->getVRegInfo(v);
        int sz = vi && vi->isPtr ? 8 : 4;
        if (local + sz > spillAreaSize) spillAreaSize = local + sz;
    }
    spillAreaSize = (spillAreaSize + 7) & ~7; // align to 8

    // Callee-Saved Area Size.
    int nCalleeSaved = (int)usedCalleeSaved.size();
    bool save_ra = !func->isLeaf;
    // check if ra is used.
    int calleeSavedSz = (nCalleeSaved + (save_ra ? 1 : 0)) * 8;
    calleeSavedSz = (calleeSavedSz + 7) & ~7;

    // Set frame layout.
    int outBase = 0;
    int spillBase = maxOutBytes;
    int allocaBase = spillBase + spillAreaSize;
    int calleeSvBase = allocaBase + func->allocaSize;
    int rawFrame = calleeSvBase + calleeSavedSz;
    int frameSize = (rawFrame + MCFunction::StackAlign - 1) & ~(MCFunction::StackAlign - 1);
    func->frameSize  = frameSize;

    rewriteOperands(func, spillBase, allocaBase);

    // Debug
    for (auto& [v, local] : spillLocal) {
        func->setSpillOffset(v, spillBase + local);
    }

/// Emit prologue in entry block
    MCBlock* entry = func->getEntryBlock();
    RvOp* first = entry ? entry->head : nullptr;

    if (frameSize > 0) {
        emitAddiSP(entry, first, -frameSize);
    }

    // Save ra if the func is non-leaf.
    if (save_ra) {
        int raOff = calleeSvBase + nCalleeSaved * 8;
        emitLS(entry, first, /*load=*/false, /*fp=*/false, /*ptr=*/true,
            Reg::ra, Reg::sp, raOff);
    }

    // Save callee-saved registers.
    // If !fp, then it's 64bits, why:
    // because in the Caller-Saved register,
    // may be a pointer or an int, and here it is uniformly processed as 64bits.
    for (int i = 0; i < nCalleeSaved; ++i) {
        int off = calleeSvBase + i * 8;
        bool fp = isFP(usedCalleeSaved[i]);
        emitLS(entry, first, /*load=*/false, fp, /*ptr=*/!fp,
            usedCalleeSaved[i], Reg::sp, off);
    }

    // These args were NOT pre-colored (to avoid caller-saved clobber across calls).
    // colorGraph assigned them callee-saved registers; we emit the copy here.
    for (auto& [v, inReg] : argIncomingReg) {
        auto* vi = func->getVRegInfo(v);
        if (!vi) continue;
        if (assignment.count(v)) {
            Reg dst = assignment[v];
            RvOp* copyOp = vi->isFloat
                ? static_cast<RvOp*>(new FMvSOp(MCOperand(dst), MCOperand(inReg)))
                : static_cast<RvOp*>(new MvOp(MCOperand(dst), MCOperand(inReg)));
            insertBefore(entry, first, copyOp);
        } else if (spillLocal.count(v)) {
            int off = spillBase + spillLocal[v];
            emitLS(entry, first, /*load=*/false, vi->isFloat, vi->isPtr,
                inReg, Reg::sp, off);
        }
        // If neither: arg was dead(no uses)，skip.
    }

    for (auto& sa : func->incomingStackArgs) {
        auto* vi = func->getVRegInfo(sa.vreg);
        bool isPtr = vi && vi->isPtr;
        int off = frameSize + sa.slotIdx * 8;

        if (assignment.count(sa.vreg)) {
            emitLS(entry, first, /*load=*/true, sa.isFloat, isPtr,
                assignment[sa.vreg], Reg::sp, off);
        } else if (spillLocal.count(sa.vreg)) {
            // load to tmp, store to spill slot.
            Reg tmp = sa.isFloat ? fspillReg : spillReg;
            int spillOff = spillBase + spillLocal[sa.vreg];
            emitLS(entry, first, /*load=*/true,  sa.isFloat, isPtr, tmp, Reg::sp, off);
            emitLS(entry, first, /*load=*/false, sa.isFloat, isPtr, tmp, Reg::sp, spillOff);
        }
        // If neither: VReg was dead(unused arg), no load needed.
    }

/// Emit epilogue before every RetOp
    func->forEachInst([&](RvOp* ret) {
        if (ret->opcode != RvOp::RetOp) return;
        MCBlock* blk = ret->parent;

        // Restore callee-saved (mirror of prologue save).
        for (int i = 0; i < nCalleeSaved; ++i) {
            int  off = calleeSvBase + i * 8;
            bool fp  = isFP(usedCalleeSaved[i]);
            emitLS(blk, ret, /*load=*/true, fp, /*ptr=*/!fp,
                   usedCalleeSaved[i], Reg::sp, off);
        }
        // Restore ra.
        if (save_ra) {
            int raOff = calleeSvBase + nCalleeSaved * 8;
            emitLS(blk, ret, /*load=*/true, /*fp=*/false, /*ptr=*/true,
                   Reg::ra, Reg::sp, raOff);
        }
        // Restore sp.
        if (frameSize > 0) {
            emitAddiSP(blk, ret, +frameSize);
        }
    });
}

void RegAlloc::run(MCFunction* func) {
    // Reset state for each function.
    interf.clear();
    spillInterf.clear();
    liveThroughCall.clear();
    assignment.clear();
    spillLocal.clear();
    priority.clear();
    pregInterf.clear();
    argIncomingReg.clear();

    func->analyzeLeaf();
    func->buildDefUseChains();
    func->computeLiveness();

    buildInterference(func);
    preColor(func);
    colorGraph(func);
    assignSpillSlots(func);
    // rewriteOperands is done in emitPrologueEpilogue.
    emitPrologueEpilogue(func);
}

} // namespace rv
} // namespace sysy
