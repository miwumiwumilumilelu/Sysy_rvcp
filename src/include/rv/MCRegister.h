#ifndef MCREGISTER_H
#define MCREGISTER_H

#include <cstdint>
#include <vector>
#include <set>

namespace sysy {

enum class PReg : int8_t {
    // rv32i (0-31)
    zero = 0, ra, sp, gp, tp, t0, t1, t2, s0, s1, a0, a1, a2, a3, a4, a5, a6, a7,
    s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, t3, t4, t5, t6,
    
    // rv32f (32-63)
    ft0 = 32, ft1, ft2, ft3, ft4, ft5, ft6, ft7, fs0, fs1, fa0, fa1, fa2, fa3, fa4, fa5, fa6, fa7,
    fs2, fs3, fs4, fs5, fs6, fs7, fs8, fs9, fs10, fs11, ft8, ft9, ft10, ft11
};

class MCRegInfo {
public:
    static const std::set<PReg> callerSaved;
    static const std::set<PReg> calleeSaved;

    static const std::vector<PReg> argRegs;
    static const std::vector<PReg> fargRegs;

    static const std::vector<PReg> allocOrder;
    static const std::vector<PReg> fallocOrder;
};

}
#endif