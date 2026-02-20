#include "rv/RegAlloc.h"
#include "rv/MCRegister.h"
#include <iostream>
#include <algorithm>
#include <set>

using namespace sysy;

// Using s10/fs10, s11/fs11 for spill.
static const std::set<PReg> RESERVED_REGS = {
    PReg::s10, PReg::s11, PReg::fs10, PReg::fs11,
    PReg::sp, PReg::gp, PReg::tp, PReg::zero, PReg::ra
};

bool isCallerSaved(PReg reg) {
    static const std::set<PReg> callerSavedSet = {
        PReg::ra, PReg::t0, PReg::t1, PReg::t2, PReg::t3, PReg::t4, PReg::t5, PReg::t6,
        PReg::a0, PReg::a1, PReg::a2, PReg::a3, PReg::a4, PReg::a5, PReg::a6, PReg::a7,
        PReg::ft0, PReg::ft1, PReg::ft2, PReg::ft3, PReg::ft4, PReg::ft5, PReg::ft6, PReg::ft7,
        PReg::ft8, PReg::ft9, PReg::ft10, PReg::ft11,
        PReg::fa0, PReg::fa1, PReg::fa2, PReg::fa3, PReg::fa4, PReg::fa5, PReg::fa6, PReg::fa7
    };

    return callerSavedSet.count(reg);
}

void RegAlloc::run(MCModule* m) {
    for (auto f : m->funcs) {
        currFunc = f;
        intervals.clear();
        liveIn.clear();
        liveOut.clear();
        def.clear();
        use.clear();
        instId.clear();
        physRegState.clear();
        allocaOffsets.clear();

        // Number the command & identify the Call command.
        numberInstructions(f);

        analyzeLiveness(f);

        linearScan();

        // Apply the assignment result, insert the Spill code.
        rewriteCode(f);
    }
}

void RegAlloc::numberInstructions(MCFunc* f) {
    int id = 0;
    callInstIds.clear();
    for (auto b : f->blks) {
        blkStart[b] = id;
        for (auto i : b->insts) {
            instId[i] = id;
            if (i->opc == MCInst::CALL) {
                callInstIds.push_back(id);
            }
            // When we find that we need to spill when allocating registers, 
            // we have to insert a load instruction between instructions 1 and 2.
            // If there is no gap, we would have to +1 all the thousands of instructions that follow,
            // which is extremely performance-consuming.
            id += 2;
        }
        blkEnd[b] = id;
    }
}

void RegAlloc::analyzeLiveness(MCFunc* f) {
    label2blk.clear();

    for (auto b : f->blks) {
        label2blk[b->name] = b;
        computeLocalLiveness(b);
    }
    computeGlobalLiveness(f);
    buildIntervals(f);
}

void RegAlloc::computeLocalLiveness(MCBlk* b) {
    def[b].clear();
    use[b].clear();
    for (auto i : b->insts) {
        for (size_t k = 0; k < i->opCnt(); k++) {
            MCOpnd& op = i->getOp(k);
            if (!op.isVReg()) continue;
            // Store/Branch isn't a def.
            bool isDef = (k == 0);
            if (i->opc == MCInst::SW || i->opc == MCInst::FSW ||
                i->opc == MCInst::BEQ || i->opc == MCInst::BNE ||
                i->opc == MCInst::BLT || i->opc == MCInst::BGE ) {
                isDef = false;
            }

            if (!isDef) {
                if (def[b].find(op.val) == def[b].end()) {
                    use[b].insert(op.val);
                } 
            } else {
                def[b].insert(op.val);
            }    
        }   
    }
}

void RegAlloc::computeGlobalLiveness(MCFunc* f) {
    bool changed = true;
    // Fixed-Point Iteration.
    while (changed) {
        changed = false;
        for (auto it = f->blks.rbegin(); it != f->blks.rend(); ++it) {
            MCBlk* b = *it;
            std::set<int> oldIn = liveIn[b];
            // LiveOut = Union(LiveIn of succ)
            liveOut[b].clear();
            if (!b->insts.empty()) {
                MCInst* last = b->insts.back();

                auto addSucc = [&](const std::string& lbl) {
                    if (label2blk.count(lbl)) {
                        MCBlk* succ = label2blk[lbl];
                        liveOut[b].insert(liveIn[succ].begin(), liveIn[succ].end());
                    }
                };

                MCBlk* nextBlk = nullptr;
                if (it.base() != f->blks.end()) {
                    nextBlk = *(it.base());
                }

                if (last->opc == MCInst::J) {
                    addSucc(last->getOp(0).label);
                } else if (last->opc == MCInst::BNE || last->opc == MCInst::BEQ ||
                           last->opc == MCInst::BLT || last->opc == MCInst::BGE) {
                    addSucc(last->getOp(last->opCnt()-1).label);
                    // Fallthrough
                    if (nextBlk) {
                        liveOut[b].insert(liveIn[nextBlk].begin(), liveIn[nextBlk].end());
                    }
                } else if (last->opc != MCInst::RET) {
                    if (nextBlk) {
                        liveOut[b].insert(liveIn[nextBlk].begin(), liveIn[nextBlk].end());
                    }
                }
            }

            // LiveIn = Use + (LiveOut - Def)
            liveIn[b] = use[b];
            for (int v : liveOut[b]) {
                if (def[b].find(v) == def[b].end()) 
                    liveIn[b].insert(v);
            }

            if (liveIn[b] != oldIn) changed = true;
        }
    }
}

void RegAlloc::buildIntervals(MCFunc* f) {
    std::map<int, Interval*> vreg2Int;

    for (auto b : f->blks) {
        int start = blkStart[b];
        int end = blkEnd[b];
        for (int v : liveOut[b]) {
            if (vreg2Int.find(v) == vreg2Int.end()) {
                vreg2Int[v] = new Interval{v, start, end, PReg::zero, false, 0, false};
                intervals.push_back(vreg2Int[v]);
            } else {
                vreg2Int[v]->start = std::min(vreg2Int[v]->start, start);
                vreg2Int[v]->end = std::max(vreg2Int[v]->end, end);
            }
        }
    }

    for (auto b : f->blks) {
        int blockFrom = blkStart[b];
        for (auto it = b->insts.rbegin(); it != b->insts.rend(); ++it) {
            MCInst* i = *it;
            int currId = instId[i];

            // Defs
            bool hasDef = false;
            int defReg = -1;
            if (i->opc != MCInst::SW && i->opc != MCInst::FSW && 
                i->opc != MCInst::BEQ && i->opc != MCInst::BNE && 
                i->opc != MCInst::BLT && i->opc != MCInst::BGE && 
                i->opc != MCInst::J && i->opc != MCInst::RET && i->opc != MCInst::CALL) {
                if (i->opCnt() > 0 && i->getOp(0).isVReg()) {
                    hasDef = true;
                    defReg = i->getOp(0).val;
                }
            }

            bool isFloat = (i->opc >= MCInst::FADD_S && i->opc <= MCInst::FSW) || i->opc == MCInst::FMV_S;

            if (hasDef) {
                if (vreg2Int.find(defReg) == vreg2Int.end()) {
                    // Dead Variable.
                    // [currId, currId + 1]
                    // Give it a tiny lifespan of +1 just to protect the site.
                    vreg2Int[defReg] = new Interval{defReg, currId, currId + 1, PReg::zero, false, 0, isFloat};
                    intervals.push_back(vreg2Int[defReg]);
                } else {
                    vreg2Int[defReg]->start = currId;
                    if (isFloat) vreg2Int[defReg]->isFloat = true;
                }
            }

            // Uses (!hasDef || k != 0)
            for (size_t k = 0; k < i->opCnt(); k++) {
                if (i->getOp(k).isVReg()) {
                    int v = i->getOp(k).val;
                    if (!hasDef || k != 0) {
                        if (vreg2Int.find(v) == vreg2Int.end()) {
                            vreg2Int[v] = new Interval{v, blockFrom, currId, PReg::zero, false, 0, isFloat};
                            intervals.push_back(vreg2Int[v]);
                        } else {
                            vreg2Int[v]->end = std::max(vreg2Int[v]->end, currId);
                            vreg2Int[v]->start = std::min(vreg2Int[v]->start, blockFrom);
                            if (isFloat) vreg2Int[v]->isFloat = true;
                        }
                    }
                }
            }
        }
    }
    
    std::sort(intervals.begin(), intervals.end(), [](Interval* a, Interval* b) {
        return a->start < b->start;
    });
}

void RegAlloc::linearScan() {
    active.clear();
    physRegState.clear();

    int stackOffset = 0;

    for (auto b : currFunc->blks) {
        for (auto inst : b->insts) {
            if (inst->opc == MCInst::ALLOCA) {
                int vreg = inst->getOp(0).val;
                int size = inst->getOp(1).val;
                allocaOffsets[vreg] = stackOffset;
                stackOffset += size;
            }
        }
    }

    for (auto i : intervals) {
        for (auto it = active.begin(); it != active.end(); ) {
            Interval* act = *it;
            if (act->end < i->start) {
                physRegState.erase(act->assigned);
                it = active.erase(it);
            } else {
                ++it;
            }
        }

        // Checking if the function call is crossed.
        bool crossesCall = false;
        for (int callId : callInstIds) {
            if (i->start < callId && i->end > callId) {
                crossesCall = true;
                break;
            }
        }

        PReg bestReg = PReg::zero;

        const auto& candidates = i->isFloat ? MCRegInfo::fallocOrder : MCRegInfo::allocOrder;
        
        for (PReg reg : candidates) {
            if (physRegState.find(reg) != physRegState.end()) continue;
            if (RESERVED_REGS.count(reg)) continue;
            if (crossesCall && isCallerSaved(reg)) continue;

            bestReg = reg;
            break;
        }

        if (bestReg != PReg::zero) {
            i->assigned = bestReg;
            i->spilled = false;
            physRegState[bestReg] = i;
            active.push_back(i);
            std::sort(active.begin(), active.end(), [](Interval* a, Interval* b) {
                return a->end < b->end;
            });
        } else {
            i->spilled = true;
            i->stackOffset = stackOffset;
            stackOffset += 4; // 32-bit = 4 bytes
        }
    }
    
    // Record the maximum stack offset,
    // to be used later for generating the Prologue, ensuring 16-byte alignment.
    currFunc->stackSize = (stackOffset + 15) / 16 * 16;
}

void RegAlloc::rewriteCode(MCFunc* f) {
    std::map<int, Interval*> vmap;
    for (auto i : intervals) vmap[i->vreg] = i;

    PReg iSpill1 = PReg::s10; 
    PReg iSpill2 = PReg::s11; 
    PReg fSpill1 = PReg::fs10;
    PReg fSpill2 = PReg::fs11;

    for (auto b : f->blks) {
        std::list<MCInst*> newInsts;
        
        for (auto inst : b->insts) {

            if (inst->opc == MCInst::ALLOCA) {
                MCOpnd& targetOp = inst->getOp(0);
                if (targetOp.isVReg() && vmap.count(targetOp.val)) {
                    Interval* it = vmap[targetOp.val];
                    int offset = allocaOffsets[targetOp.val];

                    MCInst* addi = new MCInst(MCInst::ADDI);

                    if (it->spilled) {
                        PReg scratch = iSpill1;
                        // addi s10, sp, offset
                        addi->add(MCOpnd::preg(scratch))->add(MCOpnd::preg(PReg::sp))->add(MCOpnd::imm(offset));
                        newInsts.push_back(addi);
                        
                        MCInst* st = new MCInst(MCInst::SW);
                        // sw s10, offset_v2(sp)
                        st->add(MCOpnd::preg(scratch))->add(MCOpnd::preg(PReg::sp))->add(MCOpnd::imm(it->stackOffset));
                        newInsts.push_back(st);
                    } else {
                        addi->add(MCOpnd::preg(it->assigned))->add(MCOpnd::preg(PReg::sp))->add(MCOpnd::imm(offset));
                        newInsts.push_back(addi);
                    }
                }
                continue;
            }

            // Handle Uses.
            for (size_t k = 0; k < inst->opCnt(); k++) {
                bool isDef = (k == 0);
                if (inst->opc == MCInst::SW || inst->opc == MCInst::FSW || 
                    inst->opc == MCInst::BEQ || inst->opc == MCInst::BNE || 
                    inst->opc == MCInst::BLT || inst->opc == MCInst::BGE) isDef = false;
                
                if (isDef) continue;

                MCOpnd& op = inst->getOp(k);
                if (op.isVReg() && vmap.count(op.val)) {
                    Interval* it = vmap[op.val];
                    if (it->spilled) {
                        PReg scratch = it->isFloat ? (k==1 ? fSpill2 : fSpill1) : (k==1 ? iSpill2 : iSpill1);
                        MCInst* ld = new MCInst(it->isFloat ? MCInst::FLW : MCInst::LW);
                        ld->add(MCOpnd::preg(scratch))
                          ->add(MCOpnd::preg(PReg::sp))
                          ->add(MCOpnd::imm(it->stackOffset));
                        newInsts.push_back(ld);
                        // rewrite
                        op = MCOpnd::preg(scratch);
                    } else {
                        // rewrite
                        op = MCOpnd::preg(it->assigned);
                    }
                }
            }

            newInsts.push_back(inst);

            // Handle Defs.
            if (inst->opCnt() > 0) {
                bool isDef = true;
                if (inst->opc == MCInst::SW || inst->opc == MCInst::FSW || 
                    inst->opc == MCInst::BEQ || inst->opc == MCInst::BNE || 
                    inst->opc == MCInst::BLT || inst->opc == MCInst::BGE ||
                    inst->opc == MCInst::J || inst->opc == MCInst::RET || inst->opc == MCInst::CALL) isDef = false;

                if (isDef) {
                    MCOpnd& op = inst->getOp(0);
                    if (op.isVReg() && vmap.count(op.val)) {
                        Interval* it = vmap[op.val];
                        if (it->spilled) {
                            PReg scratch = it->isFloat ? fSpill1 : iSpill1;
                            op = MCOpnd::preg(scratch);

                            MCInst* st = new MCInst(it->isFloat ? MCInst::FSW : MCInst::SW);
                            st->add(MCOpnd::preg(scratch))
                                ->add(MCOpnd::preg(PReg::sp))
                                ->add(MCOpnd::imm(it->stackOffset));
                            newInsts.push_back(st);
                        } else {
                            op = MCOpnd::preg(it->assigned);
                        }
                    }
                }
            }
        }
        b->insts = newInsts;
    }
}