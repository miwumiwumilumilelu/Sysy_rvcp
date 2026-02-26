#include "rv/Peephole.h"
#include "rv/MCModule.h"
#include "rv/MCFunction.h"
#include "rv/MCBlock.h"
#include "rv/MCInst.h"
#include "rv/MCOperand.h"
#include <iostream>

namespace sysy {

void Peephole::run(MCModule* m) {
    for (auto* func : m->funcs) {
        optimizeFunction(func);
    }
}

void Peephole::optimizeFunction(MCFunc* func) {
    for (auto* blk : func->blks) {
        optimizeBlock(blk);
    }
}

void Peephole::optimizeBlock(MCBlk* blk) {
    bool changed = true;
    // Keep iterating until no more optimizations can be applied
    while (changed) {
        changed = false;
        changed |= eliminateRedundantMove(blk->insts);
    }
}

bool Peephole::eliminateRedundantMove(std::list<MCInst*>& insts) {
    bool changed = false;

    for (auto it = insts.begin(); it != insts.end(); ) {
        MCInst* inst = *it;

        // Check for redundant moves:
        // 1. mv a0, a0 (integer)
        // 2. fmv.s fa0, fa0 (float)
        // 3. fmv fa0, fa0 (float pseudo)

        bool isRedundant = false;

        if (inst->opc == MCInst::MV) {
            // mv rd, rs - eliminate if rd == rs
            if (inst->opCnt() >= 2 &&
                inst->getOp(0).isPReg() &&
                inst->getOp(1).isPReg() &&
                inst->getOp(0).val == inst->getOp(1).val) {
                isRedundant = true;
            }
        } else if (inst->opc == MCInst::FMV_S) {
            // fmv.s rd, rs - eliminate if rd == rs
            if (inst->opCnt() >= 2 &&
                inst->getOp(0).isPReg() &&
                inst->getOp(1).isPReg() &&
                inst->getOp(0).val == inst->getOp(1).val) {
                isRedundant = true;
            }
        }

        if (isRedundant) {
            std::cerr << "  [Peephole] Eliminating redundant move: "
                      << (inst->opc == MCInst::MV ? "mv" : "fmv.s")
                      << " " << inst->getOp(0).val << ", " << inst->getOp(1).val << "\n";
            it = insts.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    return changed;
}

bool Peephole::eliminateSelfMove(std::list<MCInst*>& insts) {
    // This is now handled in eliminateRedundantMove
    return eliminateRedundantMove(insts);
}

} // namespace sysy
