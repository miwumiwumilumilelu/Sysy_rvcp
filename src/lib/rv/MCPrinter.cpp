#include "rv/MCPrinter.h"
#include "IR/Type.h"
#include "IR/Module.h"

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

static int getConstantSize(Constant* c) {
    if (isa<ConstantInt>(c) || isa<ConstantFloat>(c)) return 4;
    Type* ty = c->getType();
    int size = 4;
    while (ty->isArray()) {
        ArrayType* arrTy = static_cast<ArrayType*>(ty);
        size *= arrTy->getNumElements();
        ty = arrTy->getElementType();
    }
    return size;
}

static int printConstant(Constant* c, std::ostream& os) {
    if (isa<ConstantInt>(c)) {
        os << "    .word " << cast<ConstantInt>(c)->getValue() << "\n";
        return 4;
    } else if (isa<ConstantFloat>(c)) {
        float f = cast<ConstantFloat>(c)->getValue();
        os << "    .word " << *reinterpret_cast<int*>(&f) << "\n";
        return 4;
    } else if (isa<ConstantArray>(c)) {
        int expectedSize = getConstantSize(c);

        bool allZero = true;
        auto& consts = cast<ConstantArray>(c)->getConsts();
        for (auto elem : consts) {
            if (!isa<ConstantZero>(elem) && 
                !(isa<ConstantInt>(elem) && cast<ConstantInt>(elem)->getValue() == 0) &&
                !(isa<ConstantFloat>(elem) && cast<ConstantFloat>(elem)->getValue() == 0.0)) {
                allZero = false;
                break;
            }
        }

        if (allZero) {
            os << "    .zero " << expectedSize << "\n";
            return expectedSize;
        }

        int printedBytes = 0;
        for (auto elem : consts) {
            printedBytes += printConstant(elem, os);
        }

        if (printedBytes < expectedSize) {
            os << "    .zero " << (expectedSize - printedBytes) << "\n";
        }
        return expectedSize;
        
    } else if (isa<ConstantZero>(c)) {
        int expectedSize = getConstantSize(c);
        os << "    .zero " << expectedSize << "\n";
        return expectedSize;
    } else {
        os << "    .word 0\n";
        return 4;
    }
}

void MCPrinter::print(MCModule* module, std::ostream& os) {
    if (!module->globals.empty()) {
        os << "  .data\n";
        for (auto global : module->globals) {
            os << "  .globl " << global->getName() << "\n";
            os << "  .align 2\n";
            os << global->getName() << ":\n";

            Type* ty = global->getType();
            if (ty->isPointer()) {
                ty = static_cast<PointerType*>(ty)->getPointeeType();
            }

            int totalBytes = 4;
            Type* curTy = ty;
            while (curTy->isArray()) {
                ArrayType* arrTy = static_cast<ArrayType*>(curTy);
                totalBytes *= arrTy->getNumElements();
                curTy = arrTy->getElementType();
            }

            Constant* init = global->getInit();
            if (init && !isa<ConstantZero>(init)) {
                printConstant(init, os);
                os << "\n";
            } else {
                os << "    .zero " << totalBytes << "\n\n";
            }
        }
    }
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
        if (func->stackSize <= 2047) {
            os << "    addi sp, sp, -" << func->stackSize << "\n";
        } else {
            os << "    li t0, " << func->stackSize << "\n";
            os << "    sub sp, sp, t0\n";
        }
    }

    for (auto const& [reg, off] : func->savedRegOffsets) {
        if (off <= 2047 && off >= -2048) {
            if (static_cast<int>(reg) >= 32) {
                os << "    fsw " << getRegName(reg) << ", " << off << "(sp)\n";
            } else {
                os << "    sd " << getRegName(reg) << ", " << off << "(sp)\n";
            }
        } else {
            os << "    li t0, " << off << "\n";
            os << "    add t0, sp, t0\n";
            if (static_cast<int>(reg) >= 32) {
                os << "    fsw " << getRegName(reg) << ", 0(t0)\n";
            } else {
                os << "    sd " << getRegName(reg) << ", 0(t0)\n";
            }
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
                    if (off <= 2047 && off >= -2048) {
                        if (static_cast<int>(reg) >= 32) {
                            os << "flw " << getRegName(reg) << ", " << off << "(sp)\n    ";
                        } else {
                            os << "ld " << getRegName(reg) << ", " << off << "(sp)\n    ";
                        }
                    } else {
                        os << "li t0, " << off << "\n    ";
                        os << "add t0, sp, t0\n    ";
                        if (static_cast<int>(reg) >= 32) {
                            os << "flw " << getRegName(reg) << ", 0(t0)\n    ";
                        } else {
                            os << "ld " << getRegName(reg) << ", 0(t0)\n    ";
                        }
                    }
                }
                
                if (blk->func->stackSize > 0) {
                    if (blk->func->stackSize <= 2047) {
                        os << "addi sp, sp, " << blk->func->stackSize << "\n    ";
                    } else {
                        os << "li t0, " << blk->func->stackSize << "\n    ";
                        os << "add sp, sp, t0\n    ";
                    }
                }
            }
        }

        print(inst, os);
        os << "\n";
    }
}

void MCPrinter::print(MCInst* inst, std::ostream& os) {
    if (inst->opc == MCInst::LW || inst->opc == MCInst::SW || 
        inst->opc == MCInst::FLW || inst->opc == MCInst::FSW ||
        inst->opc == MCInst::LD || inst->opc == MCInst::SD) {
        os << getOpcName(inst->opc) << " ";
        print(inst->ops[0], os); 
        os << ", ";
        print(inst->ops[2], os); 
        os << "(";
        print(inst->ops[1], os);
        os << ")";
        return;
    }

    if (inst->opc == MCInst::CALL) {
        os << "call ";
        print(inst->ops[0], os); 
        return;
    }
    if (inst->opc == MCInst::RET) {
        os << "ret";            
        return;
    }
    if (inst->opc == MCInst::FCVT_W_S) {
        os << "fcvt.w.s ";
        print(inst->ops[0], os);
        os << ", ";
        print(inst->ops[1], os);
        os << ", rtz";
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