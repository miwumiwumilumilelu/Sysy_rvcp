#include "rv/RvOp.h"
#include <iostream>

namespace sysy {
namespace rv {

// 辅助函数：打印 MCOperand
static std::string toString(const MCOperand& op) {
    if (op.isEmpty()) return "invalid";
    if (op.isVReg()) return "%" + std::to_string(op.getVReg());
    if (op.isPReg()) return showReg(op.getPReg());
    if (op.isImm()) return std::to_string(op.getImm());
    if (op.isLabel()) return op.getLabel();
    return "?";
}

// ============================================================================
// 整数算术指令
// ============================================================================

void AddwOp::print(std::ostream& os) const {
    os << "    addw " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void SubwOp::print(std::ostream& os) const {
    os << "    subw " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void MulwOp::print(std::ostream& os) const {
    os << "    mulw " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void DivwOp::print(std::ostream& os) const {
    os << "    divw " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void RemwOp::print(std::ostream& os) const {
    os << "    remw " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

// ============================================================================
// 浮点算术指令
// ============================================================================

void FAddSOp::print(std::ostream& os) const {
    os << "    fadd.s " << toString(fd) << ", " << toString(fs1) << ", " << toString(fs2);
}

void FSubSOp::print(std::ostream& os) const {
    os << "    fsub.s " << toString(fd) << ", " << toString(fs1) << ", " << toString(fs2);
}

void FMulSOp::print(std::ostream& os) const {
    os << "    fmul.s " << toString(fd) << ", " << toString(fs1) << ", " << toString(fs2);
}

void FDivSOp::print(std::ostream& os) const {
    os << "    fdiv.s " << toString(fd) << ", " << toString(fs1) << ", " << toString(fs2);
}

// ============================================================================
// 逻辑运算指令
// ============================================================================

void AndOp::print(std::ostream& os) const {
    os << "    and " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void OrOp::print(std::ostream& os) const {
    os << "    or " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void XorOp::print(std::ostream& os) const {
    os << "    xor " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

// ============================================================================
// 移位运算指令
// ============================================================================

void SllOp::print(std::ostream& os) const {
    os << "    sll " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void SrlOp::print(std::ostream& os) const {
    os << "    srl " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void SraOp::print(std::ostream& os) const {
    os << "    sra " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

// ============================================================================
// 比较指令
// ============================================================================

void SltOp::print(std::ostream& os) const {
    os << "    slt " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

void SltuOp::print(std::ostream& os) const {
    os << "    sltu " << toString(rd) << ", " << toString(rs1) << ", " << toString(rs2);
}

// ============================================================================
// 零比较分支指令
// ============================================================================

void BeqzOp::print(std::ostream& os) const {
    os << "    beqz " << toString(rs) << ", " << target;
}

void BnezOp::print(std::ostream& os) const {
    os << "    bnez " << toString(rs) << ", " << target;
}

// ============================================================================
// 浮点类型转换指令
// ============================================================================

void FCvtWSOp::print(std::ostream& os) const {
    os << "    fcvt.w.s " << toString(rd) << ", " << toString(fs1);
}

void FCvtSWOp::print(std::ostream& os) const {
    os << "    fcvt.s.w " << toString(fd) << ", " << toString(rs1);
}

// ============================================================================
// 浮点比较指令
// ============================================================================

void FEQSOp::print(std::ostream& os) const {
    os << "    feq.s " << toString(rd) << ", " << toString(fs1) << ", " << toString(fs2);
}

void FLTSOp::print(std::ostream& os) const {
    os << "    flt.s " << toString(rd) << ", " << toString(fs1) << ", " << toString(fs2);
}

void FLESOp::print(std::ostream& os) const {
    os << "    fle.s " << toString(rd) << ", " << toString(fs1) << ", " << toString(fs2);
}

// ============================================================================
// 内存访问指令
// ============================================================================

void LwOp::print(std::ostream& os) const {
    os << "    lw " << toString(rd) << ", " << offset << "(" << toString(base) << ")";
}

void SwOp::print(std::ostream& os) const {
    os << "    sw " << toString(src) << ", " << offset << "(" << toString(base) << ")";
}

void FLwOp::print(std::ostream& os) const {
    os << "    flw " << toString(fd) << ", " << offset << "(" << toString(base) << ")";
}

void FSwOp::print(std::ostream& os) const {
    os << "    fsw " << toString(fs) << ", " << offset << "(" << toString(base) << ")";
}

// ============================================================================
// 分支指令
// ============================================================================

void BeqOp::print(std::ostream& os) const {
    os << "    beq " << toString(rs1) << ", " << toString(rs2) << ", " << target;
}

void BneOp::print(std::ostream& os) const {
    os << "    bne " << toString(rs1) << ", " << toString(rs2) << ", " << target;
}

// ============================================================================
// 跳转指令
// ============================================================================

void JOp::print(std::ostream& os) const {
    os << "    j " << label;
}

void JrOp::print(std::ostream& os) const {
    os << "    jr " << toString(rs);
}

// ============================================================================
// 其他指令
// ============================================================================

void LiOp::print(std::ostream& os) const {
    os << "    li " << toString(rd) << ", " << imm;
}

void MvOp::print(std::ostream& os) const {
    os << "    mv " << toString(rd) << ", " << toString(rs);
}

void CallOp::print(std::ostream& os) const {
    os << "    call " << target;
}

void RetOp::print(std::ostream& os) const {
    os << "    ret";
}

} // namespace rv
} // namespace sysy
