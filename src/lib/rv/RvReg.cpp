#include "rv/RvReg.h"

namespace sysy {
namespace rv {

// 溢出寄存器配置
const Reg spillReg = Reg::s10;
const Reg spillReg2 = Reg::s11;
const Reg fspillReg = Reg::fs10;
const Reg fspillReg2 = Reg::fs11;

// 整数寄存器分配顺序
// Leaf 函数优先使用临时寄存器，避免破坏栈帧
const Reg leafOrder[] = {
    Reg::a0, Reg::a1, Reg::a2, Reg::a3,
    Reg::a4, Reg::a5, Reg::a6, Reg::a7,

    Reg::t0, Reg::t1, Reg::t2, Reg::t3,
    Reg::t4, Reg::t5, Reg::t6,

    Reg::s0, Reg::s1, Reg::s2, Reg::s3,
    Reg::s4, Reg::s5, Reg::s6, Reg::s7,
    Reg::s8, Reg::s9,
};
const int leafRegCnt = sizeof(leafOrder) / sizeof(Reg);

// 非 Leaf 函数优先使用被调用者保存寄存器
const Reg normalOrder[] = {
    Reg::a0, Reg::a1, Reg::a2, Reg::a3,
    Reg::a4, Reg::a5, Reg::a6, Reg::a7,
    Reg::ra,

    Reg::t0, Reg::t1, Reg::t2, Reg::t3,
    Reg::t4, Reg::t5, Reg::t6,

    Reg::s0, Reg::s1, Reg::s2, Reg::s3,
    Reg::s4, Reg::s5, Reg::s6, Reg::s7,
    Reg::s8, Reg::s9,
};
const int normalRegCnt = sizeof(normalOrder) / sizeof(Reg);

// 参数寄存器
const Reg argRegs[] = {
    Reg::a0, Reg::a1, Reg::a2, Reg::a3,
    Reg::a4, Reg::a5, Reg::a6, Reg::a7,
};

// 调用者保存寄存器
const std::set<Reg> callerSaved = {
    Reg::t0, Reg::t1, Reg::t2, Reg::t3,
    Reg::t4, Reg::t5, Reg::t6,

    Reg::a0, Reg::a1, Reg::a2, Reg::a3,
    Reg::a4, Reg::a5, Reg::a6, Reg::a7,
    Reg::ra,

    Reg::ft0, Reg::ft1, Reg::ft2, Reg::ft3,
    Reg::ft4, Reg::ft5, Reg::ft6, Reg::ft7,
    Reg::ft8, Reg::ft9, Reg::ft10, Reg::ft11,

    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3,
    Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,
};

// 被调用者保存寄存器
const std::set<Reg> calleeSaved = {
    Reg::s0, Reg::s1, Reg::s2, Reg::s3,
    Reg::s4, Reg::s5, Reg::s6, Reg::s7,
    Reg::s8, Reg::s9, Reg::s10, Reg::s11,

    Reg::fs0, Reg::fs1, Reg::fs2, Reg::fs3,
    Reg::fs4, Reg::fs5, Reg::fs6, Reg::fs7,
    Reg::fs8, Reg::fs9, Reg::fs10, Reg::fs11,
};

// 浮点寄存器分配顺序
const Reg leafOrderf[] = {
    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3,
    Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,

    Reg::ft0, Reg::ft1, Reg::ft2, Reg::ft3,
    Reg::ft4, Reg::ft5, Reg::ft6, Reg::ft7,
    Reg::ft8, Reg::ft9, Reg::ft10, Reg::ft11,

    Reg::fs0, Reg::fs1, Reg::fs2, Reg::fs3,
    Reg::fs4, Reg::fs5, Reg::fs6, Reg::fs7,
    Reg::fs8, Reg::fs9,
};
const int leafRegCntf = sizeof(leafOrderf) / sizeof(Reg);

const Reg normalOrderf[] = {
    Reg::ft0, Reg::ft1, Reg::ft2, Reg::ft3,
    Reg::ft4, Reg::ft5, Reg::ft6, Reg::ft7,
    Reg::ft8, Reg::ft9, Reg::ft10, Reg::ft11,

    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3,
    Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,

    Reg::fs0, Reg::fs1, Reg::fs2, Reg::fs3,
    Reg::fs4, Reg::fs5, Reg::fs6, Reg::fs7,
    Reg::fs8, Reg::fs9,
};
const int normalRegCntf = sizeof(normalOrderf) / sizeof(Reg);

// 浮点参数寄存器
const Reg fargRegs[] = {
    Reg::fa0, Reg::fa1, Reg::fa2, Reg::fa3,
    Reg::fa4, Reg::fa5, Reg::fa6, Reg::fa7,
};

} // namespace rv
} // namespace sysy
