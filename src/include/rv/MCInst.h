#ifndef MCINST_H
#define MCINST_H

#include "rv/MCOperand.h"
#include <vector>

namespace sysy {

class MCBlk;

class MCInst {
public:
    enum Opc {
        // rv64i
        ADD, SUB, SLL, SRL, SRA,
        ADDI, SLLI, SRLI, SRAI,
        LD, SD,
        LUI, AUIPC,

        ADDW, SUBW, SLLW, SRLW, SRAW,
        ADDIW, SLLIW, SRLIW, SRAIW,
        MULW, DIVW, REMW,
        LW, SW,

        XOR, OR, AND, XORI, ORI, ANDI,
        SLT, SLTU, SLTI, SLTIU,
        SEQZ, SNEZ, // set eq zero

        BEQ, BNE, BLT, BGE, BLTU, BGEU,
        J, CALL, RET,

        // rv32f
        FADD_S, FSUB_S, FMUL_S, FDIV_S,
        FCVT_W_S, FCVT_S_W, FMV_W_X, FMV_X_W,
        FEQ_S, FLT_S, FLE_S,
        FLW, FSW,

        // pseudo
        LI, LA, MV, FMV_S,
        PHI, ALLOCA
    };

    // RISC-V rounding modes for float conversion instructions
    enum RoundingMode {
        RNE,  // Round to Nearest, ties to Even (default)
        RTZ,  // Round Towards Zero (C language cast behavior)
        RDN,  // Round Down (towards -∞)
        RUP,  // Round Up (towards +∞)
        RMM,  // Round to Nearest, ties to Max Magnitude
        DYN   // Dynamic (use CPU's frm register)
    };

    Opc opc;
    RoundingMode roundingMode;  // For float conversion instructions

    std::vector<MCOpnd> ops;
    MCBlk* blk;

    MCInst(Opc o, MCBlk* b = nullptr, RoundingMode rm = RNE)
        : opc(o), roundingMode(rm), blk(b) {}

    void setRoundingMode(RoundingMode rm) { roundingMode = rm; }
    RoundingMode getRoundingMode() const { return roundingMode; }

    MCInst* add(MCOpnd o) {
        ops.push_back(o);
        return this;
    }

    MCOpnd& getOp(size_t i) { return ops[i]; }
    size_t opCnt() const { return ops.size(); }
};

}

#endif
