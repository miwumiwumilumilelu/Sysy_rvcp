#ifndef REGALLOC_H
#define REGALLOC_H

#include "rv/MCFunction.h"
#include "rv/RvOp.h"
#include "rv/RvReg.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sysy {
namespace rv {

class RegAllocPass {
public:
    void run(MCFunction* func);

private:
    // Same-type interference edges.(int-int or float-float)
    // For Coloring distribution registers and Slot allocation.
    std::unordered_map<VReg, std::unordered_set<VReg>> interf;
    // Cross-type interference edges.(int-float)
    // Only for Slot allocation.
    std::unordered_map<VReg, std::unordered_set<VReg>> spillInterf;
    std::unordered_map<VReg, std::unordered_set<Reg>> pregInterf;
    // VRegs live across a CallOp: cannot use caller-saved registers.
    std::unordered_set<VReg> liveThroughCall;

    std::unordered_map<VReg, Reg> assignment;
    std::unordered_map<VReg, int> spillLocal;
    // The larger the number, the higher the priority.
    std::unordered_map<VReg, int> priority;
    // Automatic secure transfer from Caller-saved to Callee-saved.
    std::unordered_map<VReg, Reg> argIncomingReg;

    void preColor(MCFunction* func);
    void buildInterference(MCFunction* func);
    void colorGraph(MCFunction* func);
    void assignSpillSlots(MCFunction* func);
    void rewriteOperands(MCFunction* func, int spillBase, int allocaBase);
    void emitPrologueEpilogue(MCFunction* func);

    // Emit a single lw/sw/ld/sd/flw/fsw.
    // Falls back to li+add when |offset| > 2047 using Reg::t0 as scratch.
    // Inserts BEFORE pos (or appends if pos==nullptr).
    void emitLS(MCBlock* blk, RvOp* pos,
                bool isLoad, bool isFP, bool isPtr,
                Reg reg, Reg base, int offset);

    // Emit addi sp, sp, delta.  Falls back to li + add for large |delta|.
    void emitAddiSP(MCBlock* blk, RvOp* pos, int delta);
};

} // namespace rv
} // namespace sysy

#endif
