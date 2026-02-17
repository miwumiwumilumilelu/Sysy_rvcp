#include "rv/MCRegister.h"

namespace sysy {

// Caller-saved temporary registers (prefer to use, don't care if they get worn out).
const std::set<PReg> MCRegInfo::callerSaved = {
    PReg::ra, PReg::t0, PReg::t1, PReg::t2, PReg::t3, PReg::t4, PReg::t5, PReg::t6,
    PReg::a0, PReg::a1, PReg::a2, PReg::a3, PReg::a4, PReg::a5, PReg::a6, PReg::a7,
    PReg::ft0, PReg::ft1, PReg::ft2, PReg::ft3, PReg::ft4, PReg::ft5, PReg::ft6, PReg::ft7,
    PReg::ft8, PReg::ft9, PReg::ft10, PReg::ft11,
    PReg::fa0, PReg::fa1, PReg::fa2, PReg::fa3, PReg::fa4, PReg::fa5, PReg::fa6, PReg::fa7
};

// Used only when there's no other choice, 
// because using them requires pushing/popping instructions at the beginning and end of the function.
const std::set<PReg> MCRegInfo::calleeSaved = {
    PReg::sp, PReg::s0, PReg::s1, PReg::s2, PReg::s3, PReg::s4, PReg::s5, PReg::s6, PReg::s7, PReg::s8, PReg::s9, PReg::s10, PReg::s11,
    PReg::fs0, PReg::fs1, PReg::fs2, PReg::fs3, PReg::fs4, PReg::fs5, PReg::fs6, PReg::fs7, PReg::fs8, PReg::fs9, PReg::fs10, PReg::fs11
};

const std::vector<PReg> MCRegInfo::argRegs = {
    PReg::a0, PReg::a1, PReg::a2, PReg::a3, PReg::a4, PReg::a5, PReg::a6, PReg::a7
};

const std::vector<PReg> MCRegInfo::fargRegs = {
    PReg::fa0, PReg::fa1, PReg::fa2, PReg::fa3, PReg::fa4, PReg::fa5, PReg::fa6, PReg::fa7
};

const std::vector<PReg> MCRegInfo::allocOrder = {
    PReg::t0, PReg::t1, PReg::t2, PReg::t3, PReg::t4, PReg::t5, PReg::t6,
    PReg::a0, PReg::a1, PReg::a2, PReg::a3, PReg::a4, PReg::a5, PReg::a6, PReg::a7,
    PReg::s0, PReg::s1, PReg::s2, PReg::s3, PReg::s4, PReg::s5, PReg::s6, PReg::s7, PReg::s8, PReg::s9, PReg::s10, PReg::s11
};

const std::vector<PReg> MCRegInfo::fallocOrder = {
    PReg::ft0, PReg::ft1, PReg::ft2, PReg::ft3, PReg::ft4, PReg::ft5, PReg::ft6, PReg::ft7, PReg::ft8, PReg::ft9, PReg::ft10, PReg::ft11,
    PReg::fa0, PReg::fa1, PReg::fa2, PReg::fa3, PReg::fa4, PReg::fa5, PReg::fa6, PReg::fa7,
    PReg::fs0, PReg::fs1, PReg::fs2, PReg::fs3, PReg::fs4, PReg::fs5, PReg::fs6, PReg::fs7, PReg::fs8, PReg::fs9, PReg::fs10, PReg::fs11
};

} // namespace sysy