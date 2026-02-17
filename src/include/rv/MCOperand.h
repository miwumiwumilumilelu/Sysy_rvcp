#ifndef MCOPERAND_H
#define MCOPERAND_H

#include "rv/MCRegister.h"
#include <string>

namespace sysy {

class MCOpnd {
public:
    enum Ty { VREG, PREG, IMM, LBL };

    Ty ty;
    int val;
    // Using in case ty == LBL.
    std::string label;

    static MCOpnd vreg(int v) { return {VREG, v, ""}; }
    static MCOpnd preg(PReg p) { return {PREG, (int)p, ""}; }
    static MCOpnd imm(int i) { return {IMM, i, ""}; }
    static MCOpnd lbl(const std::string& l) { return {LBL, 0, l}; }

    bool isReg() const { return ty == VREG || ty == PREG; }
    bool isVReg() const { return ty == VREG; }
    bool isPReg() const { return ty == PREG; }
    bool isImm() const { return ty == IMM; }
    bool isLbl() const { return ty == LBL; }

    bool isFloatPReg() const { return ty == PREG && val >= 32; }
};

}

#endif