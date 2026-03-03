#ifndef RVREG_H
#define RVREG_H

#include <string>
#include <set>
#include <vector>

namespace sysy {
namespace rv {

// https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc
#define REGS \
  X(zero) X(ra) X(sp) X(gp) X(tp) \
  X(t0) X(t1) X(t2) X(t3) X(t4) X(t5) X(t6) \
  X(s0) X(s1) X(s2) X(s3) X(s4) X(s5) X(s6) X(s7) X(s8) X(s9) X(s10) X(s11) \
  X(a0) X(a1) X(a2) X(a3) X(a4) X(a5) X(a6) X(a7) \
  X(ft0) X(ft1) X(ft2) X(ft3) X(ft4) X(ft5) X(ft6) X(ft7) X(ft8) X(ft9) X(ft10) X(ft11) \
  X(fs0) X(fs1) X(fs2) X(fs3) X(fs4) X(fs5) X(fs6) X(fs7) X(fs8) X(fs9) X(fs10) X(fs11) \
  X(fa0) X(fa1) X(fa2) X(fa3) X(fa4) X(fa5) X(fa6) X(fa7)

enum class Reg : int {
#define X(name) name,
    REGS
#undef X
};

inline bool isFP(Reg reg) {
    return (int)reg >= (int)Reg::ft0 && (int)reg <= (int)Reg::fa7;
}

inline bool isInt(Reg reg) {
    return (int)reg >= (int)Reg::zero && (int)reg <= (int)Reg::a7;
}

inline std::string showReg(Reg reg) {
    switch (reg) {
#define X(name) case Reg::name: return #name;
        REGS
#undef X
    }
    return "<unknown>";
}

extern const Reg spillReg;
extern const Reg spillReg2;
extern const Reg fspillReg;
extern const Reg fspillReg2;

extern const Reg leafOrder[];
extern const Reg normalOrder[];
extern const Reg argRegs[];
extern const std::set<Reg> callerSaved;
extern const std::set<Reg> calleeSaved;

extern const int leafRegCnt;
extern const int normalRegCnt;

extern const Reg leafOrderf[];
extern const Reg normalOrderf[];
extern const Reg fargRegs[];

extern const int leafRegCntf;
extern const int normalRegCntf;

inline const Reg* getAllocOrder(bool isFloat, bool isLeaf) {
    return isFloat ? (isLeaf ? leafOrderf : normalOrderf)
                   : (isLeaf ? leafOrder : normalOrder);
}

inline int getAllocOrderCount(bool isFloat, bool isLeaf) {
    return isFloat ? (isLeaf ? leafRegCntf : normalRegCntf)
                   : (isLeaf ? leafRegCnt : normalRegCnt);
}

} // namespace rv
} // namespace sysy

#endif
