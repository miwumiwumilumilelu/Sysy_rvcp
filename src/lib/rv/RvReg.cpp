#include "rv/RvReg.h"

namespace sysy {
namespace rv {

/* 
RISC-V is a Load/Store architecture. 
The CPU cannot directly add or sub data in memory, 
it must first be retrieved from physical registers.
*/
// Using callee-saved registers s10/fs10 s11/fs11,
const Reg spillReg = Reg::s10;
const Reg spillReg2 = Reg::s11;
const Reg fspillReg = Reg::fs10;
const Reg fspillReg2 = Reg::fs11;

// Leaf: Prioritize the use of temporary registers to avoid breaking stack frames
const Reg leafOrder[] = {
    Reg::a0, Reg::a1, Reg::a2, Reg::a3, Reg::a4, Reg::a5, Reg::a6, Reg::a7,
    Reg::t0, Reg::t1, Reg::t2, Reg::t3, Reg::t4, Reg::t5, Reg::t6,
    Reg::s0, Reg::s1, Reg::s2, Reg::s3, Reg::s4, Reg::s5, Reg::s6, Reg::s7, Reg::s8, Reg::s9,
};
const int leafRegCnt = sizeof(leafOrder) / sizeof(Reg);

const Reg normalOrder[] = {
    Reg::a0, Reg::a1, Reg::a2, Reg::a3, Reg::a4, Reg::a5, Reg::a6, Reg::a7,
    Reg::ra,
    Reg::t0, Reg::t1, Reg::t2, Reg::t3, Reg::t4, Reg::t5, Reg::t6,
    Reg::s0, Reg::s1, Reg::s2, Reg::s3, Reg::s4, Reg::s5, Reg::s6, Reg::s7, Reg::s8, Reg::s9,
};
const int normalRegCnt = sizeof(normalOrder) / sizeof(Reg);

const Reg argRegs[] = {
    Reg::a0, Reg::a1, Reg::a2, Reg::a3,
    Reg::a4, Reg::a5, Reg::a6, Reg::a7,
};

const std::set<Reg> callerSaved = {
    Reg::t0, Reg::t1, Reg::t2, Reg::t3, Reg::t4, Reg::t5, Reg::t6,

    Reg::a0, Reg::a1, Reg::a2, Reg::a3, Reg::a4, Reg::a5, Reg::a6, Reg::a7,
    Reg::ra,

    Reg::ft0, Reg::ft1, Reg::ft2, Reg::ft3, Reg::ft4, Reg::ft5, Reg::ft6, Reg::ft7, Reg::ft8, Reg::ft9, Reg::ft10, Reg::ft11,
    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3, Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,
};

const std::set<Reg> calleeSaved = {
    Reg::s0, Reg::s1, Reg::s2, Reg::s3,
    Reg::s4, Reg::s5, Reg::s6, Reg::s7,
    Reg::s8, Reg::s9, Reg::s10, Reg::s11,

    Reg::fs0, Reg::fs1, Reg::fs2, Reg::fs3,
    Reg::fs4, Reg::fs5, Reg::fs6, Reg::fs7,
    Reg::fs8, Reg::fs9, Reg::fs10, Reg::fs11,
};

const Reg leafOrderf[] = {
    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3, Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,
    Reg::ft0, Reg::ft1, Reg::ft2, Reg::ft3, Reg::ft4, Reg::ft5, Reg::ft6, Reg::ft7, Reg::ft8, Reg::ft9, Reg::ft10, Reg::ft11,
    Reg::fs0, Reg::fs1, Reg::fs2, Reg::fs3, Reg::fs4, Reg::fs5, Reg::fs6, Reg::fs7, Reg::fs8, Reg::fs9,
};
const int leafRegCntf = sizeof(leafOrderf) / sizeof(Reg);

const Reg normalOrderf[] = {
    Reg::ft0, Reg::ft1, Reg::ft2, Reg::ft3, Reg::ft4, Reg::ft5, Reg::ft6, Reg::ft7, Reg::ft8, Reg::ft9, Reg::ft10, Reg::ft11,
    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3, Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,
    Reg::fs0, Reg::fs1, Reg::fs2, Reg::fs3, Reg::fs4, Reg::fs5, Reg::fs6, Reg::fs7, Reg::fs8, Reg::fs9,
};
const int normalRegCntf = sizeof(normalOrderf) / sizeof(Reg);

const Reg fargRegs[] = {
    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3,
    Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,
};

} // namespace rv
} // namespace sysy
