#ifndef REGALLOC_H
#define REGALLOC_H

#include "MCFunction.h"
#include "RvOp.h"
#include "RvReg.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sysy {
namespace rv {

class RegAlloc {
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
    // Narrow copy-affinity from loop-entry phi trampolines only.
    // This is a coloring bias, not real graph coalescing.
    std::unordered_map<VReg, std::unordered_map<VReg, int>> copyAffinity;
    // VRegs live across a CallOp: cannot use caller-saved registers.
    std::unordered_set<VReg> liveThroughCall;

    std::unordered_map<VReg, Reg> assignment;
    std::unordered_map<VReg, int> spillLocal;
    // The larger the number, the higher the priority.
    std::unordered_map<VReg, int> priority;
    // Automatic secure transfer from Caller-saved to Callee-saved.
    std::unordered_map<VReg, Reg> argIncomingReg;

    void preColor(MCFunction* func);
    void collectCopyAffinity(MCFunction* func);
    void buildInterference(MCFunction* func);
    void colorGraph(MCFunction* func);
    void assignSpillSlots(MCFunction* func);
    void rewriteOperands(MCFunction* func, int spillBase, int allocaBase);
    void emitPrologueEpilogue(MCFunction* func);

    // Emit a single lw/sw/ld/sd/flw/fsw.
    // Falls back to li+add when |offset| > 2047 using scratch as the address temp.
    // For int loads, pass the destination reg itself as scratch (safe: overwritten by load).
    // For stores, pass a free integer spill reg (spillReg2 for int, spillReg for fp).
    // Pro/epilogue callers leave scratch at default Reg::t0 (always safe there).
    void emitLS(MCBlock* blk, RvOp* pos,
                bool isLoad, bool isFP, bool isPtr,
                Reg reg, Reg base, int offset,
                Reg scratch = Reg::t0);

    // Emit addi sp, sp, delta.  Falls back to li + add for large |delta|.
    void emitAddiSP(MCBlock* blk, RvOp* pos, int delta);

    // Resolve read-after-write hazards in call argument moves (run after rewriteOperands).
    // Reorders or breaks cycles among the mv/fmv.s/li instructions before each call.
    //
    // Example: call f(b, a) where a->a0, b->a1 after coloring:
    //   mv a0, a1   # a0 = b  (overwrites a0, which held a)
    //   mv a1, a0   # a1 = a? NO: reads new a0 = b  <- hazard
    void fixParallelMoves(MCFunction* func);
};

} // namespace rv
} // namespace sysy

#endif
