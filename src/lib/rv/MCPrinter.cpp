#include "rv/MCPrinter.h"
#include "rv/MCModule.h"
#include "rv/MCFunction.h"
#include "rv/MCBlock.h"
#include "rv/MCInst.h"
#include "rv/MCOperand.h"
#include "rv/MCRegister.h"
#include "IR/Module.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include <cassert>
#include <algorithm>

using namespace sysy;

void MCPrinter::print(MCModule* module, std::ostream& os) {
    // Print .data section for global variables
    os << "  .data\n";

    for (auto* gv : module->globals) {
        os << "  .globl " << gv->getName() << "\n";
        os << "  .align 2\n";
        os << gv->getName() << ":\n";

        if (auto* init = gv->getInit()) {
            if (auto* ci = dyn_cast<ConstantInt>(init)) {
                os << "    .word " << ci->getValue() << "\n";
            } else if (auto* ca = dyn_cast<ConstantArray>(init)) {
                int elemSize = 4;  // Default to 4 bytes (i32)
                int totalBytes = ca->getConsts().size() * elemSize;
                os << "    .zero " << totalBytes << "\n";
            } else if (isa<ConstantZero>(init)) {
                PointerType* ptrTy = dyn_cast<PointerType>(gv->getType());
                Type* ty = ptrTy ? ptrTy->getPointeeType() : nullptr;
                if (ty) {
                    if (auto* arrTy = dyn_cast<ArrayType>(ty)) {
                        int elemSize = 4;  // Default to 4 bytes (i32)
                        int totalBytes = arrTy->getNumElements() * elemSize;
                        os << "    .zero " << totalBytes << "\n";
                    } else {
                        os << "    .word 0\n";
                    }
                } else {
                    os << "    .word 0\n";
                }
            }
        } else {
            PointerType* ptrTy = dyn_cast<PointerType>(gv->getType());
            Type* ty = ptrTy ? ptrTy->getPointeeType() : nullptr;
            if (ty) {
                if (auto* arrTy = dyn_cast<ArrayType>(ty)) {
                    int elemSize = 4;  // Default to 4 bytes (i32)
                    int totalBytes = arrTy->getNumElements() * elemSize;
                    os << "    .zero " << totalBytes << "\n";
                } else {
                    os << "    .word 0\n";
                }
            } else {
                os << "    .word 0\n";
            }
        }
    }

    // Print .text section for functions
    os << "\n  .text\n\n";

    for (size_t i = 0; i < module->funcs.size(); ++i) {
        print(module->funcs[i], os);
        // Add blank line between functions (but not after the last one)
        if (i < module->funcs.size() - 1) {
            os << "\n";
        }
    }
}

void MCPrinter::print(MCFunc* func, std::ostream& os) {
    os << "  .globl " << func->name << "\n";
    os << func->name << ":\n";

    // Calculate stack frame size
    int stackSize = func->stackSize;
    if (stackSize > 0) {
        // Allocate stack space
        os << "    addi sp, sp, -" << stackSize << "\n";
    }

    // Save callee-saved registers
    std::vector<PReg> savedRegsList;
    for (auto reg : func->savedRegs) {
        if (reg == PReg::sp) continue;  // Don't save sp
        savedRegsList.push_back(reg);
    }

    // Sort saved registers by offset (largest offset first for proper stack layout)
    std::sort(savedRegsList.begin(), savedRegsList.end(),
              [&](PReg a, PReg b) {
                  return func->savedRegOffsets[a] > func->savedRegOffsets[b];
              });

    for (auto reg : savedRegsList) {
        int offset = func->savedRegOffsets[reg];
        if ((int)reg >= 32) {
            // Float register - use fsw
            os << "    fsw " << getRegName(reg) << ", " << offset << "(sp)\n";
        } else {
            // Integer register - use sd
            os << "    sd " << getRegName(reg) << ", " << offset << "(sp)\n";
        }
    }

    // Print basic blocks
    bool isFirstBlock = true;
    for (auto* blk : func->blks) {
        print(blk, os, func, isFirstBlock);
        isFirstBlock = false;
    }
}

void MCPrinter::print(MCBlk* blk, std::ostream& os, MCFunc* func, bool isFirstBlock) {
    // Print basic block label with .L prefix (skip for first block since function entry is already there)
    if (!isFirstBlock) {
        os << ".L" << blk->name << ":\n";
    }

    // Print instructions in this block
    for (auto* inst : blk->insts) {
        // Check if this is a RET instruction and we need to print epilogue
        if (inst->opc == MCInst::RET) {
            // Print epilogue before ret
            std::vector<PReg> savedRegsList;
            for (auto reg : func->savedRegs) {
                if (reg == PReg::sp) continue;
                savedRegsList.push_back(reg);
            }

            // Sort saved registers by offset (smallest offset first for restore)
            std::sort(savedRegsList.begin(), savedRegsList.end(),
                      [&](PReg a, PReg b) {
                          return func->savedRegOffsets[a] < func->savedRegOffsets[b];
                      });

            for (auto reg : savedRegsList) {
                int offset = func->savedRegOffsets[reg];
                if ((int)reg >= 32) {
                    // Float register - use flw
                    os << "    flw " << getRegName(reg) << ", " << offset << "(sp)\n";
                } else {
                    // Integer register - use ld
                    os << "    ld " << getRegName(reg) << ", " << offset << "(sp)\n";
                }
            }

            // Restore stack pointer
            if (func->stackSize > 0) {
                os << "    addi sp, sp, " << func->stackSize << "\n";
            }
        }

        print(inst, os);
    }
}

void MCPrinter::print(MCInst* inst, std::ostream& os) {
    os << "    ";

    // Check if this is a RET instruction and we need to print epilogue
    // Note: The epilogue should be handled by the register allocator
    // or we need to track it differently. For now, just print ret.

    // Get opcode name
    const char* opcName = getOpcName(inst->opc);
    os << opcName;

    // Print operands
    for (size_t i = 0; i < inst->ops.size(); ++i) {
        if (i == 0) {
            os << " ";
        } else {
            os << ", ";
        }

        MCOpnd& opnd = inst->ops[i];

        // Special handling for load/store instructions with offset format
        // Format: offset(base) - but operands are stored as [dest, base, offset]
        if ((inst->opc == MCInst::LD || inst->opc == MCInst::SD ||
             inst->opc == MCInst::LW || inst->opc == MCInst::SW ||
             inst->opc == MCInst::FLW || inst->opc == MCInst::FSW) &&
            inst->ops.size() >= 3) {
            // For load: [dest, base, offset] -> dest, offset(base)
            // For store: [src, base, offset] -> src, offset(base)
            if (i == 0) {
                // First operand: destination/source register
                print(inst->ops[0], os);
            } else if (i == 1) {
                // Second operand: offset(base)
                os << inst->ops[2].val << "(";
                print(inst->ops[1], os);
                os << ")";
                break;  // Skip the third operand since we already printed it
            }
        } else {
            print(opnd, os);
        }
    }

    // Print rounding mode for float conversion instructions
    if (inst->opc == MCInst::FCVT_W_S || inst->opc == MCInst::FCVT_S_W) {
        const char* rmStr = getRoundingModeName(inst->getRoundingMode());
        if (rmStr) {
            os << ", " << rmStr;
        }
    }

    os << "\n";
}

void MCPrinter::print(MCOpnd& opnd, std::ostream& os) {
    switch (opnd.ty) {
        case MCOpnd::VREG:
            // Should not happen after register allocation
            os << "v" << opnd.val;
            break;
        case MCOpnd::PREG:
            os << getRegName((PReg)opnd.val);
            break;
        case MCOpnd::IMM:
            os << opnd.val;
            break;
        case MCOpnd::LBL:
            os << opnd.label;
            break;
    }
}

const char* MCPrinter::getOpcName(MCInst::Opc opc) {
    switch (opc) {
        // rv64i
        case MCInst::ADD: return "add";
        case MCInst::SUB: return "sub";
        case MCInst::SLL: return "sll";
        case MCInst::SRL: return "srl";
        case MCInst::SRA: return "sra";
        case MCInst::ADDI: return "addi";
        case MCInst::SLLI: return "slli";
        case MCInst::SRLI: return "srli";
        case MCInst::SRAI: return "srai";
        case MCInst::LD: return "ld";
        case MCInst::SD: return "sd";
        case MCInst::LUI: return "lui";
        case MCInst::AUIPC: return "auipc";

        // rv32i (for 32-bit operations)
        case MCInst::ADDW: return "addw";
        case MCInst::SUBW: return "subw";
        case MCInst::SLLW: return "sllw";
        case MCInst::SRLW: return "srlw";
        case MCInst::SRAW: return "sraw";
        case MCInst::ADDIW: return "addiw";
        case MCInst::SLLIW: return "slliw";
        case MCInst::SRLIW: return "srliw";
        case MCInst::SRAIW: return "sraiw";
        case MCInst::MULW: return "mulw";
        case MCInst::DIVW: return "divw";
        case MCInst::REMW: return "remw";
        case MCInst::LW: return "lw";
        case MCInst::SW: return "sw";

        // Logical operations
        case MCInst::XOR: return "xor";
        case MCInst::OR: return "or";
        case MCInst::AND: return "and";
        case MCInst::XORI: return "xori";
        case MCInst::ORI: return "ori";
        case MCInst::ANDI: return "andi";

        // Comparison
        case MCInst::SLT: return "slt";
        case MCInst::SLTU: return "sltu";
        case MCInst::SLTI: return "slti";
        case MCInst::SLTIU: return "sltiu";
        case MCInst::SEQZ: return "seqz";
        case MCInst::SNEZ: return "snez";

        // Branch
        case MCInst::BEQ: return "beq";
        case MCInst::BNE: return "bne";
        case MCInst::BLT: return "blt";
        case MCInst::BGE: return "bge";
        case MCInst::BLTU: return "bltu";
        case MCInst::BGEU: return "bgeu";

        // Jump and call
        case MCInst::J: return "j";
        case MCInst::CALL: return "call";
        case MCInst::RET: return "ret";

        // rv32f
        case MCInst::FADD_S: return "fadd.s";
        case MCInst::FSUB_S: return "fsub.s";
        case MCInst::FMUL_S: return "fmul.s";
        case MCInst::FDIV_S: return "fdiv.s";
        case MCInst::FCVT_W_S: return "fcvt.w.s";
        case MCInst::FCVT_S_W: return "fcvt.s.w";
        case MCInst::FMV_W_X: return "fmv.w.x";
        case MCInst::FMV_X_W: return "fmv.x.w";
        case MCInst::FEQ_S: return "feq.s";
        case MCInst::FLT_S: return "flt.s";
        case MCInst::FLE_S: return "fle.s";
        case MCInst::FLW: return "flw";
        case MCInst::FSW: return "fsw";

        // Pseudo instructions
        case MCInst::LI: return "li";
        case MCInst::LA: return "la";
        case MCInst::MV: return "mv";
        case MCInst::FMV_S: return "fmv.s";
        case MCInst::PHI: return "phi";
        case MCInst::ALLOCA: return "alloca";

        default: return "unknown";
    }
}

const char* MCPrinter::getRoundingModeName(MCInst::RoundingMode rm) {
    switch (rm) {
        case MCInst::RNE: return "rne";  // Round to Nearest, ties to Even
        case MCInst::RTZ: return "rtz";  // Round Towards Zero
        case MCInst::RDN: return "rdn";  // Round Down
        case MCInst::RUP: return "rup";  // Round Up
        case MCInst::RMM: return "rmm";  // Round to Nearest, ties to Max Magnitude
        case MCInst::DYN: return nullptr; // Dynamic (don't print, use CPU frm register)
        default: return nullptr;
    }
}

const char* MCPrinter::getRegName(PReg preg) {
    switch (preg) {
        // Integer registers (0-31)
        case PReg::zero: return "zero";
        case PReg::ra: return "ra";
        case PReg::sp: return "sp";
        case PReg::gp: return "gp";
        case PReg::tp: return "tp";
        case PReg::t0: return "t0";
        case PReg::t1: return "t1";
        case PReg::t2: return "t2";
        case PReg::s0: return "s0";
        case PReg::s1: return "s1";
        case PReg::a0: return "a0";
        case PReg::a1: return "a1";
        case PReg::a2: return "a2";
        case PReg::a3: return "a3";
        case PReg::a4: return "a4";
        case PReg::a5: return "a5";
        case PReg::a6: return "a6";
        case PReg::a7: return "a7";
        case PReg::s2: return "s2";
        case PReg::s3: return "s3";
        case PReg::s4: return "s4";
        case PReg::s5: return "s5";
        case PReg::s6: return "s6";
        case PReg::s7: return "s7";
        case PReg::s8: return "s8";
        case PReg::s9: return "s9";
        case PReg::s10: return "s10";
        case PReg::s11: return "s11";
        case PReg::t3: return "t3";
        case PReg::t4: return "t4";
        case PReg::t5: return "t5";
        case PReg::t6: return "t6";

        // Float registers (32-63)
        case PReg::ft0: return "ft0";
        case PReg::ft1: return "ft1";
        case PReg::ft2: return "ft2";
        case PReg::ft3: return "ft3";
        case PReg::ft4: return "ft4";
        case PReg::ft5: return "ft5";
        case PReg::ft6: return "ft6";
        case PReg::ft7: return "ft7";
        case PReg::fs0: return "fs0";
        case PReg::fs1: return "fs1";
        case PReg::fa0: return "fa0";
        case PReg::fa1: return "fa1";
        case PReg::fa2: return "fa2";
        case PReg::fa3: return "fa3";
        case PReg::fa4: return "fa4";
        case PReg::fa5: return "fa5";
        case PReg::fa6: return "fa6";
        case PReg::fa7: return "fa7";
        case PReg::fs2: return "fs2";
        case PReg::fs3: return "fs3";
        case PReg::fs4: return "fs4";
        case PReg::fs5: return "fs5";
        case PReg::fs6: return "fs6";
        case PReg::fs7: return "fs7";
        case PReg::fs8: return "fs8";
        case PReg::fs9: return "fs9";
        case PReg::fs10: return "fs10";
        case PReg::fs11: return "fs11";
        case PReg::ft8: return "ft8";
        case PReg::ft9: return "ft9";
        case PReg::ft10: return "ft10";
        case PReg::ft11: return "ft11";

        default: return "unknown";
    }
}
