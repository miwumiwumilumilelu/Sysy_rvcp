#include "../../include/rv/MCInvariantHoist.h"

#include <vector>

namespace sysy {
namespace rv {

static bool issameReg(const MCOperand& a, const MCOperand& b) {
    if (a.isVReg() && b.isVReg()) return a.getVReg() == b.getVReg();
    if (a.isPReg() && b.isPReg()) return a.getPReg() == b.getPReg();
    return false;
}

// Check if op's def is r.
static bool opDefines(RvOp* op, const MCOperand& r) {
    auto* def = op->getDef();
    return def && issameReg(*def, r);
}

// Check if op is using r.
static bool opUses(RvOp* op, const MCOperand& r) {
    std::vector<MCOperand*> uses;
    op->collectUses(uses);
    for (auto* u : uses)
        if (issameReg(*u, r)) return true;
    return false;
}

// Check if op operands are all VRegs.
static bool fromVReg(RvOp* op) {
    std::vector<MCOperand*> uses;
    op->collectUses(uses);
    for (auto* use : uses)
        if (!use->isVReg()) return false;
    return true;
}

bool MCInvariantHoistPass::hoistInvariant(MCFunction* func) {
    if (!func) return false;

    for (auto& bb : func->blocks) {
        MCBlock* loopBB = bb.get();
        bool selfCircle = false;
        for (auto* succ : loopBB->succs)
            if (succ == loopBB) selfCircle = true;

        // loop
        //   └── backTramp
        //           └── loop
        // bbx -> bbx_to_bbx -> bbx
        MCBlock* backTramp = nullptr;
        if (!selfCircle) {
            for (auto* succ : loopBB->succs) {
                if (succ->preds.size() == 1 && succ->preds[0] == loopBB &&
                    succ->succs.size() == 1 && succ->succs[0] == loopBB) {
                    auto* term = succ->getTerminator();
                    if (term && term->opcode == RvOp::JOp) {
                        backTramp = succ;
                        break;
                    }
                }
            }
        }
        if (!selfCircle && !backTramp) continue;

        MCBlock* pre = nullptr;
        for (auto* pred : loopBB->preds) {
            if (pred == loopBB || pred == backTramp) continue;
            if (pre) { pre = nullptr; break; }
            pre = pred;
        }
        if (!pre) continue;
        if (pre->succs.size() != 1 || pre->succs[0] != loopBB) continue;

        RvOp* preTerm = pre->getTerminator();
        if (!preTerm || preTerm->opcode != RvOp::JOp) continue;

        auto isOk = [&](RvOp* op) -> bool {
            if (!op || !op->getDef()) return false;
            switch (op->opcode) {
                case RvOp::AddiOp: case RvOp::AddOp: case RvOp::AddwOp:
                case RvOp::SubwOp: case RvOp::MulwOp:
                case RvOp::SllOp: case RvOp::SrlOp: case RvOp::SraOp:
                case RvOp::SlliwOp: case RvOp::SrliwOp: case RvOp::SraiwOp:
                case RvOp::AndiOp: case RvOp::OriOp: case RvOp::XoriOp:
                case RvOp::AndOp: case RvOp::OrOp: case RvOp::XorOp:
                case RvOp::SltOp: case RvOp::SltuOp:
                case RvOp::SltiOp: case RvOp::SltiuOp:
                case RvOp::LiOp: case RvOp::MvOp:
                    return true;
                default:
                    return false;
            }
        };

        for (RvOp* op = loopBB->head; op; op = op->next) {
            if (!isOk(op)) continue;
            auto* dst = op->getDef();
            if (!dst || !dst->isVReg()) continue;
            if (!fromVReg(op)) continue;
            std::vector<MCOperand*> workList;
            op->collectUses(workList);

            std::vector<MCBlock*> loopBlocks{loopBB};
            if (backTramp) loopBlocks.push_back(backTramp);

            // Check if Operand is defined in loop.
            bool srcDefinedInLoop = false;
            // Check whether dst is still defined by other insts within the loop.
            bool dstDefinedElsewhere = false;
            // If there are existing values, they cannot be freely hoisted.
            bool dstUsedBefore = false;
            // There is no point in hoisting it if there is no subsequent use.
            bool dstUsedAfter = false;

            for (MCBlock* lb : loopBlocks) {
                bool beforeCandidate = (lb == loopBB);
                for (RvOp* it = lb->head; it; it = it->next) {
                    if (it != op && opDefines(it, *dst))
                        dstDefinedElsewhere = true;

                    for (auto* u : workList)
                        if (u->isVReg() && opDefines(it, *u))
                            srcDefinedInLoop = true;

                    if (it == op) {
                        beforeCandidate = false;
                        continue;
                    }

                    if (opUses(it, *dst)) {
                        if (beforeCandidate) dstUsedBefore = true;
                        else dstUsedAfter = true;
                    }
                }
            }
            if (srcDefinedInLoop || dstDefinedElsewhere || dstUsedBefore || !dstUsedAfter)
                continue;

            bool dstUsedOutside = false;
            for (auto& otherPtr : func->blocks) {
                MCBlock* other = otherPtr.get();
                bool inLoopRegion = false;
                for (auto* lb : loopBlocks)
                    if (other == lb) inLoopRegion = true;
                if (inLoopRegion) continue;
                for (RvOp* it = other->head; it; it = it->next)
                    if (opUses(it, *dst)) dstUsedOutside = true;
            }
            if (dstUsedOutside) continue;

            loopBB->remove(op);
            pre->insertBefore(preTerm, op);
            return true;
        }
    }

    return false;
}

void MCInvariantHoistPass::run(MCFunction* func) {
    while (hoistInvariant(func)) {}
}

}
}
