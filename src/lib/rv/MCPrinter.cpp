#include "rv/MCPrinter.h"

using namespace sysy;

const char* MCPrinter::getRegName(PReg preg) {
    static const char* regNames[] = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
        "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
        "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7",
        "fs2", "fs3", "fs4", "fs5", "fs6", "fs7", "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"
    };
    return regNames[static_cast<int>(preg)];
}

const char* MCPrinter::getOpcName(MCInst::Opc opc) {
    static const char* opcNames[] = {
        // Uppercase indicates that these are high-level pseudo-instructions,
        // and have not yet dropped to real hardware.
        "add", "sub", "sll", "srl", "sra",
        "addi", "slli", "srli", "srai",
        "ld", "sd", "lui", "auipc",
        "addw", "subw", "sllw", "srlw", "sraw",
        "addiw", "slliw", "srliw", "sraiw",
        "mulw", "divw", "remw",
        "lw", "sw",
        "xor", "or", "and", "xori", "ori", "andi",
        "slt", "sltu", "slti", "sltiu",
        "seqz", "snez",
        "beq", "bne", "blt", "bge", "bltu", "bgeu",
        "j", "call", "ret",
        "fadd.s", "fsub.s", "fmul.s", "fdiv.s", 
        "fcvt.w.s", "fcvt.s.w", "fmv.w.x", "fmv.x.w",
        "feq.s", "flt.s", "fle.s",
        "flw", "fsw",
        "li", "la", "mv", "fmv.s",
        "PHI", "ALLOCA"
    };
    return opcNames[static_cast<int>(opc)];
}

void MCPrinter::print(MCModule* module, std::ostream& os) {
    os << "  .text\n";
    for (auto func : module->funcs) {
        print(func, os);
    }
}

void MCPrinter::print(MCFunc* func, std::ostream& os) {
    os << "\n  .globl " << func->name << "\n";
    os << "  .type " << func->name << ", @function\n";
    os << func->name << ":\n";

    if (func->stackSize > 0) {
        os << "    addi sp, sp, -" << func->stackSize << "\n";
    }

    for (auto const& [reg, off] : func->savedRegOffsets) {
        if (static_cast<int>(reg) >= 32) {
            os << "    fsw " << getRegName(reg) << ", " << off << "(sp)\n";
        } else {
            os << "    sw " << getRegName(reg) << ", " << off << "(sp)\n";
        }
    }

    for (auto blk : func->blks) {
        print(blk, os);
    }
}

void MCPrinter::print(MCBlk* blk, std::ostream& os) {
    os << blk->name << ":\n";
    for (auto inst : blk->insts) {
        os << "    ";

        if (inst->opc == MCInst::RET) {
            if (blk->func) {
                for (auto const& [reg, off] : blk->func->savedRegOffsets) {
                    if (static_cast<int>(reg) >= 32) {
                        os << "flw " << getRegName(reg) << ", " << off << "(sp)\n    ";
                    } else {
                        os << "lw " << getRegName(reg) << ", " << off << "(sp)\n    ";
                    }
                }
                
                if (blk->func->stackSize > 0) {
                    os << "addi sp, sp, " << blk->func->stackSize << "\n    ";
                }
            }
        }

        print(inst, os);
        os << "\n";
    }
}

void MCPrinter::print(MCInst* inst, std::ostream& os) {
    if (inst->opc == MCInst::LW || inst->opc == MCInst::SW || 
        inst->opc == MCInst::FLW || inst->opc == MCInst::FSW) {
        os << getOpcName(inst->opc) << " ";
        print(inst->ops[0], os); 
        os << ", ";
        print(inst->ops[2], os); 
        os << "(";
        print(inst->ops[1], os);
        os << ")";
        return;
    }

    os << getOpcName(inst->opc);
    
    if (inst->ops.empty()) return;

    os << " ";
    for (size_t i = 0; i < inst->ops.size(); ++i) {
        print(inst->ops[i], os);
        if (i != inst->ops.size() - 1) {
            os << ", ";
        }
    }
}

void MCPrinter::print(MCOpnd& opnd, std::ostream& os) {
    switch (opnd.ty) {
        case MCOpnd::VREG:
            os << "%v" << opnd.val; 
            break;
        case MCOpnd::PREG:
            os << getRegName(static_cast<PReg>(opnd.val));
            break;
        case MCOpnd::IMM:
            os << opnd.val;
            break;
        case MCOpnd::LBL:
            os << opnd.label;
            break;
    }
}