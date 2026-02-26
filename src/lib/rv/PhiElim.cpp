// Algorithm:
// 1. For each PHI instruction, create copy operations in predecessor blocks
// 2. Handle parallel copies to avoid cyclic dependencies (e.g., a=b; b=a)
// 3. Use temporary registers to break cycles
// 4. Remove all PHI instructions
//   Block2:
//     v3 = PHI(v1, Block1, v2, Block3)
//
// Becomes:
//   Block1:
//     ...existing code...
//     MV v3, v1    (inserted before terminator)
//     J Block2
//
//   Block3:
//     ...existing code...
//     MV v3, v2    (inserted before terminator)
//     J Block2

#include "rv/PhiElim.h"
#include "rv/MCModule.h"
#include "rv/MCFunction.h"
#include "rv/MCBlock.h"
#include "rv/MCInst.h"
#include "rv/MCOperand.h"
#include <map>
#include <set>
#include <algorithm>
#include <functional>

using namespace sysy;

// Represents a single copy operation: dst <- src
struct Copy {
    MCOpnd dst;
    MCOpnd src;
    MCInst* phi;           // Original PHI instruction
    MCBlk* predBlock;      // Predecessor block where copy should be inserted
    int predVReg;          // Temporary vreg for this copy (if needed)
};

// Helper function to generate appropriate copy instruction
static MCInst* generateCopy(MCOpnd dst, MCOpnd src, MCBlk* blk,
                            std::function<bool(int)> isFloatFunc) {
    // Handle immediate values - use LI instruction
    if (src.isImm()) {
        auto* li = new MCInst(MCInst::LI, blk);
        li->add(dst)->add(src);
        return li;
    }

    MCInst::Opc mvOp = MCInst::MV;

    // Determine if we need a float move instruction
    bool dstIsFloat = dst.isVReg() && isFloatFunc(dst.val);
    bool srcIsFloat = false;
    if (src.isVReg()) {
        srcIsFloat = isFloatFunc(src.val);
    } else if (src.isPReg()) {
        srcIsFloat = src.isFloatPReg();
    }

    if (dstIsFloat || srcIsFloat) {
        mvOp = MCInst::FMV_S;
    }

    auto* mv = new MCInst(mvOp, blk);
    mv->add(dst)->add(src);
    return mv;
}

void PhiElim::run(MCModule* m) {
    for (auto* func : m->funcs) {
        runOnFunc(func);
    }
}

void PhiElim::runOnFunc(MCFunc* f) {
    // Build label to block mapping
    std::map<std::string, MCBlk*> label2blk;
    for (auto* blk : f->blks) {
        label2blk[blk->name] = blk;
    }

    // Collect all PHI instructions and their copies
    // Map: predecessor block -> list of copies to insert
    std::map<MCBlk*, std::vector<Copy>> blockCopies;

    for (auto* blk : f->blks) {
        for (auto* inst : blk->insts) {
            if (inst->opc == MCInst::PHI) {
                MCOpnd phiDst = inst->getOp(0);

                // PHI operands: (val1, pred1, val2, pred2, ...)
                // Process each incoming value
                for (size_t i = 1; i < inst->opCnt(); i += 2) {
                    MCOpnd val = inst->getOp(i);
                    MCOpnd predLabel = inst->getOp(i + 1);

                    if (predLabel.isLbl() && label2blk.count(predLabel.label)) {
                        MCBlk* predBlock = label2blk[predLabel.label];

                        Copy copy;
                        copy.dst = phiDst;
                        copy.src = val;
                        copy.phi = inst;
                        copy.predBlock = predBlock;
                        copy.predVReg = -1;  // Will be assigned if needed

                        blockCopies[predBlock].push_back(copy);
                    }
                }
            }
        }
    }

    // Process copies for each block
    for (auto& entry : blockCopies) {
        MCBlk* blk = entry.first;
        auto& copies = entry.second;

        if (copies.empty()) continue;

        // Separate copies into two categories:
        // 1. Simple copies: dst is not a src in any copy in this block
        // 2. Cyclic copies: dst is also a src (need to break cycle)
        std::vector<Copy*> simpleCopies;
        std::vector<Copy*> cyclicCopies;
        std::set<int> dsts;  // All destination vregs in this block

        // Build set of all destinations
        for (auto& copy : copies) {
            if (copy.dst.isVReg()) {
                dsts.insert(copy.dst.val);
            }
        }

        // Classify copies
        for (auto& copy : copies) {
            if (copy.src.isVReg() && dsts.count(copy.src.val)) {
                // Source is also a destination in this block -> potential cycle
                cyclicCopies.push_back(&copy);
            } else {
                simpleCopies.push_back(&copy);
            }
        }

        // Find insertion point: before any branch/ret instructions
        auto insertPos = findPos(blk);

        // Lambda to check if a vreg is float
        auto isFloatCheck = [this, f](int vr) { return isFloat(vr, f); };

        // Process simple copies first
        for (auto* copy : simpleCopies) {
            auto* copyInst = generateCopy(copy->dst, copy->src, blk, isFloatCheck);
            blk->insts.insert(insertPos, copyInst);
        }

        // Process cyclic copies using temporary registers
        if (!cyclicCopies.empty()) {
            // Build a mapping from original dst to temporary
            std::map<int, MCOpnd> dst2tmp;

            for (auto* copy : cyclicCopies) {
                if (copy->dst.isVReg() && !dst2tmp.count(copy->dst.val)) {
                    int tmpVReg = f->maxVReg++;
                    dst2tmp[copy->dst.val] = MCOpnd::vreg(tmpVReg);
                }
            }

            for (auto* copy : cyclicCopies) {
                if (copy->dst.isVReg() && dst2tmp.count(copy->dst.val)) {
                    auto* save = generateCopy(dst2tmp[copy->dst.val], copy->dst, blk, isFloatCheck);
                    blk->insts.insert(insertPos, save);
                }
            }

            for (auto* copy : cyclicCopies) {
                MCOpnd actualSrc = copy->src;

                // If src is a vreg that's also a destination, use the saved value
                if (copy->src.isVReg() && dst2tmp.count(copy->src.val)) {
                    actualSrc = dst2tmp[copy->src.val];
                }

                auto* copyInst = generateCopy(copy->dst, actualSrc, blk, isFloatCheck);
                blk->insts.insert(insertPos, copyInst);
            }
        }
    }

    for (auto* blk : f->blks) {
        auto it = blk->insts.begin();
        while (it != blk->insts.end()) {
            if ((*it)->opc == MCInst::PHI) {
                it = blk->insts.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool PhiElim::isFloat(int vr, MCFunc* f) {
    // Check if this virtual register holds a float value
    // We need to look at the instruction that defined it
    for (auto* blk : f->blks) {
        for (auto* inst : blk->insts) {
            if (inst->opc == MCInst::PHI) {
                // Skip PHI instructions during this check
                continue;
            }

            // Check if this instruction defines the vreg
            if (inst->opCnt() > 0 && inst->getOp(0).isVReg() &&
                inst->getOp(0).val == vr) {
                // Float-producing instructions
                if (inst->opc == MCInst::FADD_S ||
                    inst->opc == MCInst::FSUB_S ||
                    inst->opc == MCInst::FMUL_S ||
                    inst->opc == MCInst::FDIV_S ||
                    inst->opc == MCInst::FCVT_S_W ||
                    inst->opc == MCInst::FLW ||
                    inst->opc == MCInst::FMV_S) {
                    return true;
                }
                return false;
            }
        }
    }

    return false;
}

std::list<MCInst*>::iterator PhiElim::findPos(MCBlk* b) {
    // Find the position to insert copy instructions
    // We want to insert before any branch or return instruction
    // If no such instruction exists, insert at the end

    if (b->insts.empty()) {
        return b->insts.end();
    }

    auto it = b->insts.end();
    --it;

    // Check if the last instruction is a terminator
    if ((*it)->opc == MCInst::J ||
        (*it)->opc == MCInst::RET ||
        (*it)->opc == MCInst::CALL ||
        ((*it)->opc >= MCInst::BEQ && (*it)->opc <= MCInst::BGEU)) {
        // Insert before this terminator
        return it;
    }

    return b->insts.end();
}
