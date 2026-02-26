#include "rv/RegAlloc.h"
#include "rv/MCModule.h"
#include "rv/MCFunction.h"
#include "rv/MCBlock.h"
#include "rv/MCInst.h"
#include "rv/MCOperand.h"
#include "rv/MCRegister.h"
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <iostream>

using namespace sysy;

void RegAlloc::run(MCModule* m) {
    for (auto* func : m->funcs) {
        currFunc = func;

        // Reset state for this function
        intervals.clear();
        callInstIds.clear();
        allocaOffsets.clear();
        liveIn.clear();
        liveOut.clear();
        def.clear();
        use.clear();
        instId.clear();
        blkStart.clear();
        blkEnd.clear();
        label2blk.clear();
        state = AllocState();

        // Phase 1: Number instructions and analyze liveness
        numberInstructions(func);
        analyzeLiveness(func);
        buildIntervals(func);

        // Phase 2: Allocate registers using linear scan
        allocateRegisters();

        // Phase 3: Rewrite program with allocated registers
        rewriteProgram();
    }
}

void RegAlloc::numberInstructions(MCFunc* f) {
    int id = 0;

    // Build label to block mapping
    for (auto* blk : f->blks) {
        label2blk[blk->name] = blk;
    }

    // Number instructions in each block
    for (auto* blk : f->blks) {
        blkStart[blk] = id;
        for (auto* inst : blk->insts) {
            instId[inst] = id++;
        }
        blkEnd[blk] = id - 1;
    }
}

void RegAlloc::computeLocalLiveness(MCBlk* b) {
    def[b].clear();
    use[b].clear();

    for (auto* inst : b->insts) {
        // Collect used virtual registers (operands)
        for (size_t i = 0; i < inst->opCnt(); ++i) {
            auto& opnd = inst->getOp(i);
            if (opnd.isVReg()) {
                // Clean the float flag bit (bit 16) to get the actual vreg number
                int vreg = opnd.val & ~0x10000;
                // Only add to use if not defined in this block
                if (def[b].find(vreg) == def[b].end()) {
                    use[b].insert(vreg);
                }
            }
        }

        // Collect defined virtual registers (def is usually first operand)
        // Skip ALLOCA instructions - they define stack slots, not vregs
        if (inst->opc != MCInst::ALLOCA && inst->opCnt() > 0) {
            auto& defOpnd = inst->getOp(0);
            if (defOpnd.isVReg()) {
                // Clean the float flag bit (bit 16)
                int vreg = defOpnd.val & ~0x10000;
                def[b].insert(vreg);
            }
        }

        // Track call instructions
        if (inst->opc == MCInst::CALL) {
            callInstIds.push_back(instId[inst]);
        }
    }
}

void RegAlloc::computeGlobalLiveness(MCFunc* f) {
    // Initialize liveOut for all blocks
    for (auto* blk : f->blks) {
        liveOut[blk].clear();
        liveIn[blk].clear();
    }

    // Iterative dataflow analysis
    bool changed = true;
    while (changed) {
        changed = false;

        for (auto* blk : f->blks) {
            // Compute liveOut = union of successors' liveIn
            std::set<int> newLiveOut;

            // Get successors from branch instructions
            MCInst* branchInst = nullptr;
            if (!blk->insts.empty()) {
                branchInst = blk->insts.back();
            }

            if (branchInst) {
                if (branchInst->opc == MCInst::J || branchInst->opc == MCInst::CALL) {
                    // Unconditional branch - single successor
                    if (branchInst->opCnt() > 0 && branchInst->getOp(0).isLbl()) {
                        std::string label = branchInst->getOp(0).label;
                        if (label2blk.count(label)) {
                            MCBlk* succ = label2blk[label];
                            for (int vreg : liveIn[succ]) {
                                newLiveOut.insert(vreg);
                            }
                        }
                    }
                } else if (branchInst->opc >= MCInst::BEQ && branchInst->opc <= MCInst::BGEU) {
                    // Conditional branch - two successors
                    // First successor is the branch target
                    if (branchInst->opCnt() > 2 && branchInst->getOp(2).isLbl()) {
                        std::string label = branchInst->getOp(2).label;
                        if (label2blk.count(label)) {
                            MCBlk* succ = label2blk[label];
                            for (int vreg : liveIn[succ]) {
                                newLiveOut.insert(vreg);
                            }
                        }
                    }
                    // Second successor is the fall-through block
                    // Find the next block in the function
                    auto it = std::find(f->blks.begin(), f->blks.end(), blk);
                    if (it != f->blks.end() && ++it != f->blks.end()) {
                        MCBlk* fallthrough = *it;
                        for (int vreg : liveIn[fallthrough]) {
                            newLiveOut.insert(vreg);
                        }
                    }
                }
            }

            // liveIn = use ∪ (liveOut - def)
            std::set<int> newLiveIn = use[blk];
            for (int vreg : newLiveOut) {
                if (def[blk].find(vreg) == def[blk].end()) {
                    newLiveIn.insert(vreg);
                }
            }

            if (newLiveOut != liveOut[blk] || newLiveIn != liveIn[blk]) {
                changed = true;
                liveOut[blk] = newLiveOut;
                liveIn[blk] = newLiveIn;
            }
        }
    }
}

void RegAlloc::analyzeLiveness(MCFunc* f) {
    // Compute local liveness for each block
    for (auto* blk : f->blks) {
        computeLocalLiveness(blk);
    }

    // Compute global liveness
    computeGlobalLiveness(f);
}

void RegAlloc::buildIntervals(MCFunc* f) {
    std::map<int, Interval*> vreg2Interval;

    // Helper function to check if virtual register is float
    auto isFloatVReg = [](int vreg) {
        return (vreg & 0x10000) != 0;
    };

    // Helper function to check if instruction is float
    auto isFloatInst = [](MCInst* inst) {
        // Most float instructions output to float registers
        // Exceptions:
        // - FCVT_W_S outputs to integer register
        // - FMV_X_W outputs to integer register
        if (inst->opc == MCInst::FCVT_W_S || inst->opc == MCInst::FMV_X_W) {
            return false;  // Result is in integer register
        }
        return inst->opc >= MCInst::FADD_S && inst->opc <= MCInst::FSW ||
               inst->opc == MCInst::FMV_S;
    };

    // First pass: collect all virtual register definitions
    for (auto* blk : f->blks) {
        for (auto* inst : blk->insts) {
            if (inst->opc == MCInst::ALLOCA) {
                // ALLOCA instructions define stack slots
                if (inst->opCnt() > 0 && inst->getOp(0).isVReg()) {
                    int vreg = inst->getOp(0).val;
                    int offset = state.stackOffset;
                    state.stackOffset += 8; // Assume 8-byte stack slots
                    allocaOffsets[vreg] = offset;
                }
                continue;
            }

            // Check if this instruction defines a virtual register
            if (inst->opCnt() > 0) {
                auto& defOpnd = inst->getOp(0);
                if (defOpnd.isVReg()) {
                    int vreg = defOpnd.val;
                    // Clean the float flag bit to get the actual vreg number
                    int cleanVreg = vreg & ~0x10000;
                    if (!vreg2Interval.count(cleanVreg)) {
                        Interval* interval = new Interval();
                        interval->vreg = cleanVreg;
                        interval->start = instId[inst];
                        interval->end = instId[inst];
                        interval->assigned = PReg::zero; // Unassigned
                        interval->spilled = false;
                        interval->stackOffset = 0;
                        // Check if this is a float virtual register or float instruction
                        interval->isFloat = isFloatVReg(vreg) || isFloatInst(inst);
                        interval->defInst = inst;

                        intervals.push_back(interval);
                        vreg2Interval[cleanVreg] = interval;
                    }
                }
            }
        }
    }

    // Second pass: extend intervals based on liveness
    for (auto* blk : f->blks) {
        // Work backwards through the block
        std::set<int> currentLive = liveOut[blk];

        auto instIt = blk->insts.rbegin();
        while (instIt != blk->insts.rend()) {
            MCInst* inst = *instIt;
            int id = instId[inst];

            // Extend intervals for all currently live variables
            for (int vreg : currentLive) {
                if (vreg2Interval.count(vreg)) {
                    Interval* interval = vreg2Interval[vreg];
                    if (interval->end < id) {
                        interval->end = id;
                    }
                }
            }

            // Process the instruction
            if (inst->opc != MCInst::ALLOCA && inst->opCnt() > 0) {
                auto& defOpnd = inst->getOp(0);
                if (defOpnd.isVReg()) {
                    int vreg = defOpnd.val;
                    // Remove from current live set
                    currentLive.erase(vreg);
                }
            }

            // Add uses to current live set
            for (size_t i = 0; i < inst->opCnt(); ++i) {
                auto& opnd = inst->getOp(i);
                if (opnd.isVReg()) {
                    // Clean the float flag bit (bit 16) to get the actual vreg number
                    int vreg = opnd.val & ~0x10000;
                    currentLive.insert(vreg);
                }
            }

            ++instIt;
        }
    }

    // Sort intervals by start position
    std::sort(intervals.begin(), intervals.end(),
        [](const Interval* a, const Interval* b) {
            return a->start < b->start;
        });
}

bool RegAlloc::isLeafFunction() const {
    return callInstIds.empty();
}

void RegAlloc::allocateRegisters() {
    bool isLeaf = isLeafFunction();

    // Get the appropriate allocation order
    const auto& allocOrder = isLeaf ? MCRegInfo::leafAllocOrder : MCRegInfo::normalAllocOrder;
    const auto& fallocOrder = isLeaf ? MCRegInfo::leafFallocOrder : MCRegInfo::normalFallocOrder;

    // Track which physical registers are available using vectors to preserve order
    std::vector<PReg> regOrder;
    std::vector<bool> regAvailable;
    for (auto preg : allocOrder) {
        regOrder.push_back(preg);
        regAvailable.push_back(preg != PReg::sp && preg != PReg::zero && preg != PReg::ra);
    }

    std::cerr << "  [Alloc] Available int regs (first 10): ";
    int count = 0;
    for (size_t i = 0; i < regOrder.size(); ++i) {
        if (count++ >= 10) break;
        std::cerr << (int)regOrder[i] << "(" << (regAvailable[i] ? "avail" : "unavail") << ") ";
    }
    std::cerr << "\n";

    std::vector<PReg> fregOrder;
    std::vector<bool> fregAvailable;
    for (auto preg : fallocOrder) {
        fregOrder.push_back(preg);
        fregAvailable.push_back(true);
    }

    // Helper lambda to find and mark a register as available
    auto freeReg = [&](PReg reg, bool isFloat) {
        if (isFloat) {
            auto it = std::find(fregOrder.begin(), fregOrder.end(), reg);
            if (it != fregOrder.end()) {
                fregAvailable[std::distance(fregOrder.begin(), it)] = true;
            }
        } else {
            auto it = std::find(regOrder.begin(), regOrder.end(), reg);
            if (it != regOrder.end()) {
                regAvailable[std::distance(regOrder.begin(), it)] = true;
            }
        }
    };

    // Helper lambda to find and allocate the first available register
    auto allocFirstAvail = [&](bool isFloat) -> PReg {
        if (isFloat) {
            for (size_t i = 0; i < fregOrder.size(); ++i) {
                if (fregAvailable[i]) {
                    fregAvailable[i] = false;
                    return fregOrder[i];
                }
            }
        } else {
            for (size_t i = 0; i < regOrder.size(); ++i) {
                if (regAvailable[i]) {
                    regAvailable[i] = false;
                    return regOrder[i];
                }
            }
        }
        return PReg::zero; // No available register
    };

    // Helper lambda to check if any register is available
    auto hasRegAvailable = [&](bool isFloat) -> bool {
        if (isFloat) {
            for (bool avail : fregAvailable) {
                if (avail) return true;
            }
        } else {
            for (bool avail : regAvailable) {
                if (avail) return true;
            }
        }
        return false;
    };

    // Active intervals
    std::vector<Interval*> active;

    // Process intervals in order of increasing start point
    for (auto* interval : intervals) {
        // Expire old intervals
        auto it = active.begin();
        while (it != active.end()) {
            Interval* activeInterval = *it;
            if (activeInterval->end < interval->start) {
                // Free the register
                freeReg(activeInterval->assigned, activeInterval->isFloat);
                it = active.erase(it);
            } else {
                ++it;
            }
        }

        // Try to allocate a register
        if (hasRegAvailable(interval->isFloat)) {
            // Allocate the first available register (preserves allocOrder)
            PReg reg = allocFirstAvail(interval->isFloat);
            interval->assigned = reg;
            interval->spilled = false;

            std::cerr << "  [Alloc] vreg " << interval->vreg << " [" << interval->start << "," << interval->end << "] isFloat=" << interval->isFloat << " -> " << (int)reg << (interval->isFloat ? "f" : "") << "\n";

            // Insert into active list, sorted by end point
            active.insert(std::upper_bound(active.begin(), active.end(), interval,
                [](const Interval* a, const Interval* b) {
                    return a->end < b->end;
                }), interval);
        } else {
            // Need to spill
            // Spill the interval with the furthest end point
            if (!active.empty()) {
                Interval* toSpill = active.back();
                if (toSpill->end > interval->end) {
                    // Spill the active interval and allocate current
                    freeReg(toSpill->assigned, toSpill->isFloat);
                    toSpill->spilled = true;
                    toSpill->stackOffset = state.stackOffset;
                    state.stackOffset += 8;
                    active.pop_back();

                    // Allocate register to current interval
                    PReg reg = allocFirstAvail(interval->isFloat);
                    interval->assigned = reg;
                    interval->spilled = false;

                    active.insert(std::upper_bound(active.begin(), active.end(), interval,
                        [](const Interval* a, const Interval* b) {
                            return a->end < b->end;
                        }), interval);
                } else {
                    // Spill current interval
                    interval->spilled = true;
                    interval->stackOffset = state.stackOffset;
                    state.stackOffset += 8;
                }
            } else {
                // No active intervals, must spill current
                interval->spilled = true;
                interval->stackOffset = state.stackOffset;
                state.stackOffset += 8;
            }
        }
    }

    // Update function stack size
    currFunc->stackSize = state.stackOffset;

    // For non-leaf functions, save ra register
    if (!isLeaf) {
        currFunc->savedRegs.insert(PReg::ra);
        // Allocate space for ra on stack
        // Note: savedRegOffsets should be set, but if not, calculate here
        if (currFunc->savedRegOffsets.find(PReg::ra) == currFunc->savedRegOffsets.end()) {
            currFunc->savedRegOffsets[PReg::ra] = state.stackOffset;
            state.stackOffset += 8;
        }
        // Update stack size again
        currFunc->stackSize = state.stackOffset;
    }

    // Track saved registers for callee-saved registers that are used
    for (auto* interval : intervals) {
        if (!interval->spilled && MCRegInfo::calleeSaved.count(interval->assigned)) {
            currFunc->savedRegs.insert(interval->assigned);
            // Allocate space for callee-saved register
            if (currFunc->savedRegOffsets.find(interval->assigned) == currFunc->savedRegOffsets.end()) {
                currFunc->savedRegOffsets[interval->assigned] = state.stackOffset;
                state.stackOffset += 8;
            }
        }
    }

    // Update final stack size with 16-byte alignment (RISC-V ABI requirement)
    // RISC-V ABI requires stack pointer to be 16-byte aligned at function call
    if (state.stackOffset % 16 != 0) {
        state.stackOffset = (state.stackOffset / 16 + 1) * 16;
    }
    currFunc->stackSize = state.stackOffset;
}

void RegAlloc::rewriteProgram() {
    // Build vreg to interval mapping
    std::map<int, Interval*> vreg2Interval;
    for (auto* interval : intervals) {
        vreg2Interval[interval->vreg] = interval;
    }

    // Rewrite each instruction
    for (auto* blk : currFunc->blks) {
        std::list<MCInst*> newInsts;

        for (auto* inst : blk->insts) {
            // Skip ALLOCA instructions - they don't generate code
            if (inst->opc == MCInst::ALLOCA) {
                continue;
            }

            // Collect operands that need reloads
            std::vector<int> reloads;
            for (size_t i = 0; i < inst->opCnt(); ++i) {
                auto& opnd = inst->getOp(i);
                if (opnd.isVReg()) {
                    int cleanVreg = opnd.val & ~0x10000;
                    if (vreg2Interval.count(cleanVreg)) {
                        Interval* interval = vreg2Interval[cleanVreg];
                        if (interval->spilled) {
                            reloads.push_back(i);
                        }
                    }
                }
            }

            // Generate reload instructions before the current instruction
            for (int opIdx : reloads) {
                auto& opnd = inst->getOp(opIdx);
                int cleanVreg = opnd.val & ~0x10000;
                Interval* interval = vreg2Interval[cleanVreg];

                // Allocate a temporary register for reload
                // Use t0 as a temporary scratch register
                MCInst* reload = new MCInst(MCInst::LD, blk);
                reload->add(MCOpnd::preg(PReg::t0));

                // Calculate stack offset: fp - offset
                // For now, use sp-based addressing
                reload->add(MCOpnd::preg(PReg::sp));
                reload->add(MCOpnd::imm(-interval->stackOffset - 16)); // Adjust for saved registers

                newInsts.push_back(reload);

                // Replace operand with temporary register
                opnd = MCOpnd::preg(PReg::t0);
            }

            // Rewrite virtual registers to physical registers
            for (size_t i = 0; i < inst->opCnt(); ++i) {
                auto& opnd = inst->getOp(i);
                if (opnd.isVReg()) {
                    int cleanVreg = opnd.val & ~0x10000;
                    if (vreg2Interval.count(cleanVreg)) {
                        Interval* interval = vreg2Interval[cleanVreg];
                        if (!interval->spilled) {
                            opnd = MCOpnd::preg(interval->assigned);
                        }
                    }
                }
            }

            // Check if the result needs to be spilled
            if (inst->opCnt() > 0) {
                auto& defOpnd = inst->getOp(0);
                if (defOpnd.isPReg()) {
                    PReg preg = (PReg)defOpnd.val;
                    // Find the interval for this register
                    for (auto* interval : intervals) {
                        if (!interval->spilled && interval->assigned == preg) {
                            // This is the destination interval, check if it needs spilling
                            // Actually, we need to track which interval this instruction defines
                            break;
                        }
                    }
                }
            }

            newInsts.push_back(inst);
        }

        // Replace block instructions with new instructions
        blk->insts = newInsts;
    }
}
