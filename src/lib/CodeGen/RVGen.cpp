#include "CodeGen/RVGen.h"
#include "IR/Instruction.h"
#include "IR/Type.h"
#include <iostream>

using namespace sysy;

RVGen::RVGen(Module *module) : TheModule(module) {}

void RVGen::emit(const std::string &inst) {
    // Use tab for indentation to conform to assembly standards
    AsmStream << "\t" << inst << "\n";
}

void RVGen::emitLabel(const std::string &label) {
    // Labels should not be indented
    AsmStream << label << ":\n";
}

void RVGen::generate() {
    AsmStream << "\t.text\n";
    for (auto func : TheModule->getFunctions()) {
        genFunction(func);
    }
}

void RVGen::allocateStackSlots(Function *func) {
    StackSlots.clear();
    CurrentStackSize = 0;
    
    // Iterate over all blocks in the function body
    // Since CFG is flattened, we just iterate the linear list of blocks
    for (auto bb : func->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            // Allocate stack space for any instruction that produces a value (non-void)
            if (!inst->getType()->isVoid()) {
                CurrentStackSize += 4; // Assume 4 bytes for all types (i32)
                StackSlots[inst] = -CurrentStackSize; 
            }
        }
    }
    // Align stack size to 16 bytes (RISC-V ABI requirement)
    CurrentStackSize = (CurrentStackSize + 15) / 16 * 16;
}

void RVGen::assignLabels(Function *func) {
    BBLabelMap.clear();
    int cnt = 0;
    for (auto bb : func->getBody()->getBlocks()) {
        std::string name = bb->getName();
        if (name.empty()) name = "bb";
        // Generate unique label: .L<funcName>_<blockName>_<id>
        std::string label = ".L" + func->getName() + "_" + name + "_" + std::to_string(cnt++);
        BBLabelMap[bb] = label;
    }
}

void RVGen::loadValueToReg(Value *val, const std::string &regName) {
    if (auto constInt = dynamic_cast<ConstantInt*>(val)) {
        // Load immediate value
        emit("li " + regName + ", " + std::to_string(constInt->getValue()));
    } else {
        // Load variable from stack
        if (StackSlots.find(val) == StackSlots.end()) {
            // Global variables or errors (not handled yet)
            return;
        }
        int offset = StackSlots[val];
        // Load from offset relative to s0 (Frame Pointer)
        emit("lw " + regName + ", " + std::to_string(offset) + "(s0)");
    }
}

void RVGen::storeRegToStack(const std::string &regName, Value *destVal) {
    if (StackSlots.count(destVal)) {
        int offset = StackSlots[destVal];
        emit("sw " + regName + ", " + std::to_string(offset) + "(s0)");
    }
}

void RVGen::genFunction(Function *func) {
    // 1. Preparation: Assign labels and calculate stack size
    assignLabels(func);
    allocateStackSlots(func);

    // Function declaration
    AsmStream << "\t.globl " << func->getName() << "\n";
    AsmStream << "\t.type " << func->getName() << ", @function\n";
    emitLabel(func->getName());

    // 2. Prologue
    // Save Return Address (ra) and Old Frame Pointer (s0)
    int frameSize = CurrentStackSize + 16; 
    frameSize = (frameSize + 15) / 16 * 16;

    emit("addi sp, sp, -" + std::to_string(frameSize));
    emit("sd ra, " + std::to_string(frameSize - 8) + "(sp)");
    emit("sd s0, " + std::to_string(frameSize - 16) + "(sp)");
    emit("addi s0, sp, " + std::to_string(frameSize));

    // 3. Body Generation
    for (auto bb : func->getBody()->getBlocks()) {
        genBasicBlock(bb);
    }
}

void RVGen::genBasicBlock(BasicBlock *bb) {
    // Emit the label for this block
    if (BBLabelMap.count(bb)) {
        emitLabel(BBLabelMap[bb]);
    }
    // Generate instructions
    for (auto inst : bb->getInstructions()) {
        genInstruction(inst);
    }
}

void RVGen::genInstruction(Instruction *inst) {
    switch (inst->getOpID()) {
        case Instruction::Ret: {
            // Handle return value
            if (inst->getNumOperands() > 0) {
                loadValueToReg(inst->getOperand(0), "a0");
            }
            // Epilogue: Restore stack and registers
            int frameSize = (CurrentStackSize + 16 + 15) / 16 * 16;
            emit("ld ra, " + std::to_string(frameSize - 8) + "(sp)");
            emit("ld s0, " + std::to_string(frameSize - 16) + "(sp)");
            emit("addi sp, sp, " + std::to_string(frameSize));
            emit("ret");
            break;
        }
        case Instruction::Add:
        case Instruction::Sub:
        case Instruction::Mul:
        case Instruction::Div: {
            loadValueToReg(inst->getOperand(0), "t0");
            loadValueToReg(inst->getOperand(1), "t1");
            
            std::string op;
            // Use word-suffix instructions for 32-bit integers in RV64
            if (inst->getOpID() == Instruction::Add) op = "addw";
            else if (inst->getOpID() == Instruction::Sub) op = "subw";
            else if (inst->getOpID() == Instruction::Mul) op = "mulw";
            else if (inst->getOpID() == Instruction::Div) op = "divw";

            emit(op + " t0, t0, t1");
            storeRegToStack("t0", inst);
            break;
        }
        case Instruction::Alloca: {
            // Space is already reserved in allocateStackSlots.
            // No runtime instruction needed for basic scalar alloca.
            break; 
        }
        case Instruction::Load: {
            // Load val = load ptr
            // In our simple model, ptr (AllocaInst) represents the stack slot itself.
            Value *ptr = inst->getOperand(0);
            if (StackSlots.count(ptr)) {
                int offset = StackSlots[ptr];
                emit("lw t0, " + std::to_string(offset) + "(s0)");
                storeRegToStack("t0", inst);
            }
            break;
        }
        case Instruction::Store: {
            // Store val, ptr
            loadValueToReg(inst->getOperand(0), "t0"); // val -> t0
            Value *ptr = inst->getOperand(1);
            if (StackSlots.count(ptr)) {
                int offset = StackSlots[ptr];
                emit("sw t0, " + std::to_string(offset) + "(s0)");
            }
            break;
        }
        case Instruction::ICmp: {
            loadValueToReg(inst->getOperand(0), "t0");
            loadValueToReg(inst->getOperand(1), "t1");
            
            if (auto icmp = dynamic_cast<ICmpInst*>(inst)) {
                auto pred = icmp->getPredicate();
                if (pred == ICmpInst::SGT) {
                    emit("sgt t0, t0, t1");
                } else if (pred == ICmpInst::SLT) {
                    emit("slt t0, t0, t1");
                } else if (pred == ICmpInst::EQ) {
                    emit("subw t0, t0, t1");
                    emit("seqz t0, t0");     // Set t0 = 1 if t0 == 0 (equal)
                } else if (pred == ICmpInst::NE) {
                    emit("subw t0, t0, t1");
                    emit("snez t0, t0");     // Set t0 = 1 if t0 != 0 (not equal)
                } else if (pred == ICmpInst::SGE) { 
                    // a >= b  <=>  !(a < b)
                    emit("slt t0, t0, t1");
                    emit("xori t0, t0, 1");
                } else if (pred == ICmpInst::SLE) { 
                    // a <= b  <=>  !(a > b)
                    emit("sgt t0, t0, t1");
                    emit("xori t0, t0, 1");
                }
            }
            storeRegToStack("t0", inst);
            break;
        }
        case Instruction::Br: {
            if (inst->getNumOperands() == 1) {
                // Unconditional branch
                BasicBlock* target = dynamic_cast<BasicBlock*>(inst->getOperand(0));
                emit("j " + BBLabelMap[target]);
            } else {
                // Conditional branch
                loadValueToReg(inst->getOperand(0), "t0"); // Condition
                BasicBlock* trueBB = dynamic_cast<BasicBlock*>(inst->getOperand(1));
                BasicBlock* falseBB = dynamic_cast<BasicBlock*>(inst->getOperand(2));
                
                // If condition is true (non-zero), jump to true block
                emit("bnez t0, " + BBLabelMap[trueBB]);
                // Otherwise fall through to jump to false block
                emit("j " + BBLabelMap[falseBB]);
            }
            break;
        }
        default:
            emit("# Unknown Instruction");
    }
}