#include "rv/InstSelector.h"
#include "rv/MCModule.h"
#include "rv/MCFunction.h"
#include "rv/MCBlock.h"
#include "rv/MCInst.h"
#include "rv/MCOperand.h"
#include "rv/MCRegister.h"
#include "IR/Module.h"
#include "IR/Instruction.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include <cassert>

using namespace sysy;

MCModule* InstSelector::run(Module* irModule) {
    curMCMod = new MCModule();

    // Copy global variables
    for (auto* gv : irModule->getGlobals()) {
        curMCMod->globals.push_back(gv);
    }

    // Process each function
    for (auto* func : irModule->getFunctions()) {
        selectFunction(func);
    }

    return curMCMod;
}

MCOpnd InstSelector::createVReg(bool isFloat) {
    MCOpnd opnd = MCOpnd::vreg(nextVRegNo++);
    // Float virtual registers are distinguished by setting a high bit
    // This is a simple approach; alternatively, we could use separate counters
    if (isFloat) {
        opnd.val |= 0x10000;  // Set bit 16 to indicate float virtual register
    }
    return opnd;
}

MCOpnd InstSelector::getOpnd(Value* val) {
    // If we already have an operand for this value, return it
    if (val2opnd.count(val)) {
        return val2opnd[val];
    }

    // Handle Argument - return physical register directly if possible
    if (auto* arg = dyn_cast<Argument>(val)) {
        int argNo = arg->getArgNo();
        Type* ty = arg->getType();

        // Arguments are passed in a0-a7 for integers, fa0-fa7 for floats
        if (ty->isInt() || ty->isPointer()) {
            if (argNo < 8) {
                // Return the physical register directly, no MV needed
                PReg preg = (PReg)((int)PReg::a0 + argNo);
                MCOpnd opnd = MCOpnd::preg(preg);
                val2opnd[val] = opnd;
                return opnd;
            }
        } else if (ty->isFloat()) {
            if (argNo < 8) {
                // Return the physical register directly, no MV needed
                PReg preg = (PReg)((int)PReg::fa0 + argNo);
                MCOpnd opnd = MCOpnd::preg(preg);
                val2opnd[val] = opnd;
                return opnd;
            }
        }
        // For arguments beyond the first 8, they're on stack - need to load them
        // This will be handled in selectFunction
    }

    // Handle ConstantInt
    if (auto* c = dyn_cast<ConstantInt>(val)) {
        return MCOpnd::imm(c->getValue());
    }

    // Handle ConstantFloat
    if (auto* cf = dyn_cast<ConstantFloat>(val)) {
        // For float constants, we need to load them into a register
        // Create a virtual register for the float value
        MCOpnd vreg = createVReg(true);  // float virtual register

        // Load the float bits as an integer immediate
        float fval = cf->getValue();
        int bits = *(int*)&fval;

        // Load the bits into an integer register, then move to float register
        MCOpnd tmpVReg = createVReg();  // integer virtual register
        curMCBlk->push((new MCInst(MCInst::LI))->add(tmpVReg)->add(MCOpnd::imm(bits)));
        curMCBlk->push((new MCInst(MCInst::FMV_W_X))->add(vreg)->add(tmpVReg));

        val2opnd[val] = vreg;
        return vreg;
    }

    // Handle ConstantZero
    if (isa<ConstantZero>(val)) {
        return MCOpnd::imm(0);
    }

    // Handle GlobalVariable
    if (auto* gv = dyn_cast<GlobalVariable>(val)) {
        return MCOpnd::lbl(gv->getName());
    }

    // Handle BasicBlock (for branch targets)
    if (auto* bb = dyn_cast<BasicBlock>(val)) {
        assert(bbMap.count(bb) && "Basic block not yet mapped!");
        return MCOpnd::lbl(bbMap[bb]->name);
    }

    // For instructions, we should have already created the operand
    // If not, create a new virtual register
    MCOpnd vreg = createVReg();
    val2opnd[val] = vreg;
    return vreg;
}

void InstSelector::selectFunction(Function* func) {
    // Create MCFunction
    curMCFunc = new MCFunc(func->getName(), curMCMod);
    curMCMod->add(curMCFunc);

    // Reset state for this function
    val2opnd.clear();
    bbMap.clear();
    nextVRegNo = 0;

    // Process arguments
    // For arguments beyond the first 8, they're passed on the stack
    // We need to load them from the stack
    // Stack layout: [saved ra][saved fp][args > 7][local vars]
    // The caller puts args > 7 at offset 0 from the stack pointer

    for (auto* arg : func->getArgs()) {
        int argNo = arg->getArgNo();
        Type* ty = arg->getType();

        // Arguments 0-7 are in registers (a0-a7 or fa0-fa7)
        if (argNo < 8) {
            // Don't create any instruction here
            // getOpnd will return the physical register directly
            // This avoids generating redundant MV instructions
            if (ty->isInt() || ty->isPointer()) {
                PReg preg = (PReg)((int)PReg::a0 + argNo);
                val2opnd[arg] = MCOpnd::preg(preg);
            } else if (ty->isFloat()) {
                PReg preg = (PReg)((int)PReg::fa0 + argNo);
                val2opnd[arg] = MCOpnd::preg(preg);
            }
        } else {
            // Arguments 8+ are on the stack
            // Load them into virtual registers
            MCOpnd vreg = createVReg();
            val2opnd[arg] = vreg;

            // Generate load instruction at function entry
            // These will be added to the entry block later
            // Stack offset: each argument is 8 bytes (we use 8-byte alignment)
            // int offset = stackArgOffset + (argNo - 8) * 8;

            // We need to defer this until we have the entry block
            // Store the info to process later
            // TODO: Handle stack arguments
        }
    }

    // Create MCBlocks for all basic blocks
    for (auto* bb : func->getBody()->getBlocks()) {
        auto* mcblk = new MCBlk(bb->getName(), curMCFunc);
        curMCFunc->add(mcblk);
        bbMap[bb] = mcblk;
    }

    // Process each basic block
    for (auto* bb : func->getBody()->getBlocks()) {
        selectBlock(bb);
    }

    // Update maxVReg for register allocation
    curMCFunc->maxVReg = nextVRegNo;
}

void InstSelector::selectBlock(BasicBlock* bb) {
    curMCBlk = bbMap[bb];

    // Process each instruction
    for (auto* inst : bb->getInstructions()) {
        selectInstruction(inst);
    }
}

void InstSelector::selectInstruction(Instruction* inst) {
    switch (inst->getOpID()) {
        case Instruction::Ret:
            selectRet(cast<ReturnInst>(inst));
            break;
        case Instruction::Br:
            selectBranch(cast<BranchInst>(inst));
            break;
        case Instruction::Add:
        case Instruction::Sub:
        case Instruction::Mul:
        case Instruction::Div:
        case Instruction::Mod:
            selectBinaryOp(cast<BinaryInst>(inst));
            break;
        case Instruction::FAdd:
        case Instruction::FSub:
        case Instruction::FMul:
        case Instruction::FDiv:
            selectFBinaryOp(cast<BinaryInst>(inst));
            break;
        case Instruction::Alloca:
            selectAlloca(cast<AllocaInst>(inst));
            break;
        case Instruction::Load:
            selectLoad(cast<LoadInst>(inst));
            break;
        case Instruction::Store:
            selectStore(cast<StoreInst>(inst));
            break;
        case Instruction::ICmp:
            selectICmp(cast<ICmpInst>(inst));
            break;
        case Instruction::FCmp:
            selectFCmp(cast<FCmpInst>(inst));
            break;
        case Instruction::Call:
            selectCall(cast<CallInst>(inst));
            break;
        case Instruction::SIToFP:
        case Instruction::FPToSI:
            selectCast(cast<CastInst>(inst));
            break;
        case Instruction::GetElementPtr:
            selectGetElementPtr(cast<GetElementPtrInst>(inst));
            break;
        case Instruction::Phi:
            selectPhi(cast<PhiInst>(inst));
            break;
        case Instruction::If:
        case Instruction::While:
        case Instruction::Break:
        case Instruction::Continue:
            // These are high-level constructs that should have been lowered
            // If we encounter them, it's an error
            assert(false && "High-level instruction should have been lowered!");
            break;
        default:
            assert(false && "Unknown instruction type!");
            break;
    }
}

void InstSelector::selectRet(ReturnInst* inst) {
    Value* val = inst->getOperand(0);

    if (val && !val->getType()->isVoid()) {
        MCOpnd opnd = getOpnd(val);

        // Move return value to a0 or fa0
        if (val->getType()->isFloat()) {
            if (!opnd.isPReg() || (PReg)opnd.val != PReg::fa0) {
                auto* mv = new MCInst(MCInst::FMV_S, curMCBlk);
                mv->add(MCOpnd::preg(PReg::fa0))->add(opnd);
                curMCBlk->push(mv);
            }
        } else {
            if (!opnd.isPReg() || (PReg)opnd.val != PReg::a0) {
                if (opnd.isImm()) {
                    // For immediate values, use LI instruction
                    auto* li = new MCInst(MCInst::LI, curMCBlk);
                    li->add(MCOpnd::preg(PReg::a0))->add(opnd);
                    curMCBlk->push(li);
                } else if (opnd.isPReg() && (PReg)opnd.val >= PReg::ft0) {
                    // If operand is a float register, convert to int first
                    MCOpnd tmpVReg = createVReg();  // integer vreg
                    auto* fcvt = new MCInst(MCInst::FCVT_W_S, curMCBlk);
                    fcvt->add(tmpVReg)->add(opnd);
                    curMCBlk->push(fcvt);

                    // Then move to a0
                    auto* mv = new MCInst(MCInst::MV, curMCBlk);
                    mv->add(MCOpnd::preg(PReg::a0))->add(tmpVReg);
                    curMCBlk->push(mv);
                } else {
                    // For registers, use MV instruction
                    auto* mv = new MCInst(MCInst::MV, curMCBlk);
                    mv->add(MCOpnd::preg(PReg::a0))->add(opnd);
                    curMCBlk->push(mv);
                }
            }
        }
    }

    // Generate return instruction
    auto* ret = new MCInst(MCInst::RET, curMCBlk);
    curMCBlk->push(ret);
}

void InstSelector::selectBranch(BranchInst* inst) {
    MCOpnd cond = getOpnd(inst->getOperand(0));
    MCOpnd ifTrue = getOpnd(inst->getOperand(1));
    MCOpnd ifFalse = getOpnd(inst->getOperand(2));

    auto* br = new MCInst(MCInst::BNE, curMCBlk);
    br->add(cond)->add(MCOpnd::imm(0))->add(ifTrue);
    curMCBlk->push(br);

    auto* j = new MCInst(MCInst::J, curMCBlk);
    j->add(ifFalse);
    curMCBlk->push(j);
}

void InstSelector::selectBinaryOp(BinaryInst* inst) {
    MCOpnd lhs = getOpnd(inst->getOperand(0));
    MCOpnd rhs = getOpnd(inst->getOperand(1));
    MCOpnd dst = createVReg();

    val2opnd[inst] = dst;

    MCInst::Opc opc;
    switch (inst->getOpID()) {
        case Instruction::Add: opc = MCInst::ADD; break;
        case Instruction::Sub: opc = MCInst::SUB; break;
        case Instruction::Mul: opc = MCInst::MULW; break;
        case Instruction::Div: opc = MCInst::DIVW; break;
        case Instruction::Mod: opc = MCInst::REMW; break;
        default: assert(false && "Unknown binary op!");
    }

    auto* binop = new MCInst(opc, curMCBlk);
    binop->add(dst)->add(lhs)->add(rhs);
    curMCBlk->push(binop);
}

void InstSelector::selectFBinaryOp(BinaryInst* inst) {
    MCOpnd lhs = getOpnd(inst->getOperand(0));
    MCOpnd rhs = getOpnd(inst->getOperand(1));
    MCOpnd dst = createVReg();

    val2opnd[inst] = dst;

    MCInst::Opc opc;
    switch (inst->getOpID()) {
        case Instruction::FAdd: opc = MCInst::FADD_S; break;
        case Instruction::FSub: opc = MCInst::FSUB_S; break;
        case Instruction::FMul: opc = MCInst::FMUL_S; break;
        case Instruction::FDiv: opc = MCInst::FDIV_S; break;
        default: assert(false && "Unknown float binary op!");
    }

    auto* binop = new MCInst(opc, curMCBlk);
    binop->add(dst)->add(lhs)->add(rhs);
    curMCBlk->push(binop);
}

void InstSelector::selectAlloca(AllocaInst* inst) {
    // Allocate space on stack
    MCOpnd vreg = createVReg();
    val2opnd[inst] = vreg;

    auto* alloca = new MCInst(MCInst::ALLOCA, curMCBlk);
    alloca->add(vreg)->add(MCOpnd::imm(4)); // TODO: get actual size
    curMCBlk->push(alloca);
}

void InstSelector::selectLoad(LoadInst* inst) {
    MCOpnd ptr = getOpnd(inst->getOperand(0));
    MCOpnd dst = createVReg();

    val2opnd[inst] = dst;

    MCInst::Opc opc;
    Type* elemTy = cast<PointerType>(inst->getOperand(0)->getType())->getPointeeType();
    if (elemTy->isFloat()) {
        opc = MCInst::FLW;
    } else {
        opc = MCInst::LW;
    }

    auto* load = new MCInst(opc, curMCBlk);
    load->add(dst)->add(ptr)->add(MCOpnd::imm(0));
    curMCBlk->push(load);
}

void InstSelector::selectStore(StoreInst* inst) {
    MCOpnd val = getOpnd(inst->getOperand(0));
    MCOpnd ptr = getOpnd(inst->getOperand(1));

    MCInst::Opc opc;
    Type* elemTy = inst->getOperand(0)->getType();
    if (elemTy->isFloat()) {
        opc = MCInst::FSW;
    } else {
        opc = MCInst::SW;
    }

    auto* store = new MCInst(opc, curMCBlk);
    store->add(val)->add(ptr)->add(MCOpnd::imm(0));
    curMCBlk->push(store);
}

void InstSelector::selectICmp(ICmpInst* inst) {
    MCOpnd lhs = getOpnd(inst->getOperand(0));
    MCOpnd rhs = getOpnd(inst->getOperand(1));
    MCOpnd dst = createVReg();

    val2opnd[inst] = dst;

    MCInst::Opc opc;
    switch (inst->getPredicate()) {
        case ICmpInst::EQ: opc = MCInst::BEQ; break;
        case ICmpInst::NE: opc = MCInst::BNE; break;
        case ICmpInst::SLT: opc = MCInst::SLT; break;
        case ICmpInst::SGT:
            // SLT r1, r2, r3 => r1 = (r2 < r3)
            // For SGT, we need SLT r1, r3, r2
            std::swap(lhs, rhs);
            opc = MCInst::SLT;
            break;
        case ICmpInst::SLE:
            // SLE = !(SLT with swapped operands)
            // We can use SLT and then invert, or use branches
            // For simplicity, use SLT with swapped operands and invert
            std::swap(lhs, rhs);
            opc = MCInst::SLT;
            break;
        case ICmpInst::SGE:
            // SGE = !(SLT)
            // Use SLT and then invert
            opc = MCInst::SLT;
            break;
        default: assert(false && "Unknown icmp predicate!");
    }

    auto* cmp = new MCInst(opc, curMCBlk);
    cmp->add(dst)->add(lhs)->add(rhs);
    curMCBlk->push(cmp);
}

void InstSelector::selectFCmp(FCmpInst* inst) {
    MCOpnd lhs = getOpnd(inst->getOperand(0));
    MCOpnd rhs = getOpnd(inst->getOperand(1));
    MCOpnd dst = createVReg();

    val2opnd[inst] = dst;

    MCInst::Opc opc;
    switch (inst->getPredicate()) {
        case FCmpInst::OEQ: opc = MCInst::FEQ_S; break;
        case FCmpInst::OLT: opc = MCInst::FLT_S; break;
        case FCmpInst::OLE: opc = MCInst::FLE_S; break;
        default: assert(false && "Unknown fcmp predicate!");
    }

    auto* cmp = new MCInst(opc, curMCBlk);
    cmp->add(dst)->add(lhs)->add(rhs);
    curMCBlk->push(cmp);
}

void InstSelector::selectCall(CallInst* inst) {
    Function* callee = inst->getFunction();

    // Move arguments to a0-a7 / fa0-fa7
    // Note: operand 0 is the callee function, actual arguments start from operand 1
    int argRegIdx = 0;
    for (int i = 1; i < inst->getNumOperands(); ++i) {
        Value* arg = inst->getOperand(i);
        MCOpnd argOpnd = getOpnd(arg);
        Type* argTy = arg->getType();

        if (argRegIdx < 8) {
            PReg targetReg;
            if (argTy->isFloat()) {
                targetReg = (PReg)((int)PReg::fa0 + argRegIdx);
            } else {
                targetReg = (PReg)((int)PReg::a0 + argRegIdx);
            }

            // Only generate MV if the argument is not already in the target register
            if (!argOpnd.isPReg() || (PReg)argOpnd.val != targetReg) {
                if (argOpnd.isImm()) {
                    // For immediate values, use LI instruction
                    auto* li = new MCInst(MCInst::LI, curMCBlk);
                    li->add(MCOpnd::preg(targetReg))->add(argOpnd);
                    curMCBlk->push(li);
                } else {
                    // For registers, use MV instruction
                    MCInst::Opc mvOp = argTy->isFloat() ? MCInst::FMV_S : MCInst::MV;
                    auto* mv = new MCInst(mvOp, curMCBlk);
                    mv->add(MCOpnd::preg(targetReg))->add(argOpnd);
                    curMCBlk->push(mv);
                }
            }
            argRegIdx++;
        } else {
            // Arguments beyond 8 go on stack
            // TODO: Handle stack arguments
        }
    }

    // Generate call instruction
    auto* call = new MCInst(MCInst::CALL, curMCBlk);
    call->add(MCOpnd::lbl(callee->getName()));
    curMCBlk->push(call);

    // Call result is already in a0/fa0, no need to move it
    // Just record that the call result is in the return register
    if (!inst->getType()->isVoid()) {
        PReg retReg = inst->getType()->isFloat() ? PReg::fa0 : PReg::a0;
        val2opnd[inst] = MCOpnd::preg(retReg);
    }
}

void InstSelector::selectCast(CastInst* inst) {
    Value* val = inst->getOperand(0);
    MCOpnd opnd = getOpnd(val);

    MCInst::Opc opc;
    if (inst->getOpID() == Instruction::SIToFP) {
        opc = MCInst::FCVT_S_W;
        // fcvt.s.w: int -> float, result in float register
        MCOpnd dst = createVReg(true);  // float vreg
        val2opnd[inst] = dst;

        auto* cast = new MCInst(opc, curMCBlk);
        cast->add(dst)->add(opnd);
        curMCBlk->push(cast);
    } else {
        // FPToSI: float -> int (use RTZ rounding mode for C semantics)
        opc = MCInst::FCVT_W_S;
        // fcvt.w.s: float -> int, result in INTEGER register!
        MCOpnd dst = createVReg();  // int vreg
        val2opnd[inst] = dst;

        // Use RTZ (round towards zero) for C language cast semantics
        auto* fcvt = new MCInst(opc, curMCBlk, MCInst::RTZ);
        fcvt->add(dst)->add(opnd);
        curMCBlk->push(fcvt);
    }
}

void InstSelector::selectGetElementPtr(GetElementPtrInst* inst) {
    Value* base = inst->getOperand(0);
    Value* index = inst->getOperand(1);

    MCOpnd baseOpnd = getOpnd(base);
    MCOpnd indexOpnd = getOpnd(index);
    MCOpnd dst = createVReg();

    val2opnd[inst] = dst;

    // Calculate offset: base + index * element_size
    // For simplicity, assume element_size is 4 (int or float)
    // TODO: Get actual element size from type

    // First, multiply index by element size
    MCOpnd temp = createVReg();
    auto* mul = new MCInst(MCInst::ADDIW, curMCBlk);
    mul->add(temp)->add(indexOpnd)->add(MCOpnd::imm(2)); // Multiply by 4
    curMCBlk->push(mul);

    // Then add to base
    auto* add = new MCInst(MCInst::ADD, curMCBlk);
    add->add(dst)->add(baseOpnd)->add(temp);
    curMCBlk->push(add);
}

void InstSelector::selectPhi(PhiInst* inst) {
    MCOpnd dst = createVReg();
    val2opnd[inst] = dst;

    // PHI nodes should be handled during SSA destruction
    // For now, just create a pseudo PHI instruction
    auto* phi = new MCInst(MCInst::PHI, curMCBlk);
    phi->add(dst);

    // Add incoming values
    // Note: This is a simplified implementation
    // Real PHI handling requires more complex logic during SSA destruction
    for (int i = 0; i < inst->getNumOperands(); i += 2) {
        Value* val = inst->getOperand(i);
        BasicBlock* bb = cast<BasicBlock>(inst->getOperand(i + 1));
        phi->add(getOpnd(val))->add(MCOpnd::lbl(bbMap[bb]->name));
    }

    curMCBlk->push(phi);
}
