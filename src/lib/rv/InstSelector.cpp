#include "rv/InstSelector.h"
#include "IR/Instruction.h"
#include <iostream>

using namespace sysy;

static int getTypeSize(Type* ty) {
    if (auto arrTy = dyn_cast<ArrayType>(ty)) {
        return arrTy->getNumElements() * getTypeSize(arrTy->getElementType());
    }
    return 4;
}

MCModule* InstSelector::run(Module* irModule) {
    curMCMod = new MCModule();

    curMCMod->globals = irModule->getGlobals();
    for (auto func : irModule->getFunctions()) {
        if (!func->getBody()->getBlocks().empty()) {
            selectFunction(func);
        }
    }
    return curMCMod;
}

MCOpnd InstSelector::createVReg() {
    int regNo = nextVRegNo++;
    curMCFunc->maxVReg = nextVRegNo;
    return MCOpnd::vreg(regNo);
}

MCOpnd InstSelector::getOpnd(Value* val) {
    if (val2opnd.find(val) != val2opnd.end()) {
        return val2opnd[val];   
    }

    if (auto ci = dyn_cast<ConstantInt>(val)) {
        int imm = ci->getValue();
        MCOpnd vreg = createVReg();
        auto liInst = new MCInst(MCInst::LI);
        liInst->add(vreg)->add(MCOpnd::imm(imm));
        curMCBlk->push(liInst);

        val2opnd[val] = vreg;
        return vreg;
    }

    if (auto cf = dyn_cast<ConstantFloat>(val)) {
        float fval = cf->getValue();
        // Treat the binary bits of float as int.
        int imm = *reinterpret_cast<int*>(&fval);
        MCOpnd intReg = createVReg();
        MCOpnd floatReg = createVReg();

        // First load these 32 bits of binary into the integer register.
        // LI intReg, imm
        auto liInst = new MCInst(MCInst::LI);
        liInst->add(intReg)->add(MCOpnd::imm(imm));
        curMCBlk->push(liInst);

        // Then the bits in the integer register are moved to the floating-point register.
        // FMV_W_X floatReg, intReg
        auto fmvInst = new MCInst(MCInst::FMV_W_X);
        fmvInst->add(floatReg)->add(intReg);
        curMCBlk->push(fmvInst);

        val2opnd[val] = floatReg;
        return floatReg;
    }

    if (auto gv = dyn_cast<GlobalVariable>(val)) {
        MCOpnd vreg = createVReg();
        // Using LA to load the label of the global variable.
        auto laInst = new MCInst(MCInst::LA);
        laInst->add(vreg)->add(MCOpnd::lbl(gv->getName()));
        curMCBlk->push(laInst);

        val2opnd[val] = vreg;
        return vreg;
    }

    // TODO: such as function parameters, first allocate a virtual register as a fallback
    MCOpnd vreg = createVReg();
    val2opnd[val] = vreg;
    return vreg;
}

void InstSelector::selectFunction(Function* func) {
    curMCFunc = new MCFunc(func->getName());
    curMCMod->add(curMCFunc);
    // Each function has an independent virtual register number, starting at 0.
    nextVRegNo = 0;
    val2opnd.clear();
    bbMap.clear();

    for (auto bb : func->getBody()->getBlocks()) {
        MCBlk* mcBlk = new MCBlk(".L" + func->getName() + "_" + bb->getName());
        bbMap[bb] = mcBlk;
        curMCFunc->add(mcBlk);
    }

    curMCBlk = bbMap[func->getBody()->getEntryBlock()];
    int intCnt = 0;
    int floatCnt = 0;
    for (auto arg : func->getArgs()) {
        MCOpnd vreg = createVReg();
        val2opnd[arg] = vreg;
        if (arg->getType()->isFloat()) {
            PReg preg = static_cast<PReg>(static_cast<int>(PReg::fa0) + floatCnt++);
            curMCBlk->push((new MCInst(MCInst::FMV_S))->add(vreg)->add(MCOpnd::preg(preg)));
        } else {
            PReg preg = static_cast<PReg>(static_cast<int>(PReg::a0) + intCnt++);
            curMCBlk->push((new MCInst(MCInst::MV))->add(vreg)->add(MCOpnd::preg(preg)));
        }
    }

    for (auto bb : func->getBody()->getBlocks()) {
        selectBlock(bb);
    }
}

void InstSelector::selectBlock(BasicBlock* bb) {
    curMCBlk = bbMap[bb];
    for (auto inst : bb->getInstructions()) {
        selectInstruction(inst);
    }
}

void InstSelector::selectInstruction(Instruction* inst) {
    switch (inst->getOpID()) {
        case Instruction::Add:
        case Instruction::Sub:
        case Instruction::Mul:
        case Instruction::Div:
        case Instruction::Mod:
        case Instruction::FAdd:
        case Instruction::FSub:
        case Instruction::FMul:
        case Instruction::FDiv: {
            MCOpnd rd = getOpnd(inst);
            MCOpnd rs1 = getOpnd(inst->getOperand(0));
            MCOpnd rs2 = getOpnd(inst->getOperand(1));

            MCInst::Opc op = MCInst::ADDW;
            switch(inst->getOpID()) {
                case Instruction::Add: op = MCInst::ADDW; break;
                case Instruction::Sub: op = MCInst::SUBW; break;
                case Instruction::Mul: op = MCInst::MULW; break;
                case Instruction::Div: op = MCInst::DIVW; break;
                case Instruction::Mod: op = MCInst::REMW; break;
                case Instruction::FAdd: op = MCInst::FADD_S; break;
                case Instruction::FSub: op = MCInst::FSUB_S; break;
                case Instruction::FMul: op = MCInst::FMUL_S; break;
                case Instruction::FDiv: op = MCInst::FDIV_S; break;
                default: break;
            }
            curMCBlk->push((new MCInst(op))->add(rd)->add(rs1)->add(rs2));
            break;
        }
        case Instruction::SIToFP:
            curMCBlk->push((new MCInst(MCInst::FCVT_S_W))->add(getOpnd(inst))->add(getOpnd(inst->getOperand(0))));
            break;
        case Instruction::FPToSI:
            curMCBlk->push((new MCInst(MCInst::FCVT_W_S))->add(getOpnd(inst))->add(getOpnd(inst->getOperand(0))));
            break;
        case Instruction::ICmp: {
            auto cmp = cast<ICmpInst>(inst);
            MCOpnd rd = getOpnd(inst);
            MCOpnd lhs = getOpnd(cmp->getOperand(0));
            MCOpnd rhs = getOpnd(cmp->getOperand(1));

            switch(cmp->getPredicate()) {
                case ICmpInst::EQ: // a == b -> xor t, a, b; seqz rd, t
                    curMCBlk->push((new MCInst(MCInst::XOR))->add(rd)->add(lhs)->add(rhs));
                    curMCBlk->push((new MCInst(MCInst::SEQZ))->add(rd)->add(rd));
                    break;
                case ICmpInst::NE: // a != b -> xor t, a, b; snez rd, t
                    curMCBlk->push((new MCInst(MCInst::XOR))->add(rd)->add(lhs)->add(rhs));
                    curMCBlk->push((new MCInst(MCInst::SNEZ))->add(rd)->add(rd));
                    break;
                case ICmpInst::SLT: // a < b -> slt rd, a, b
                    curMCBlk->push((new MCInst(MCInst::SLT))->add(rd)->add(lhs)->add(rhs));
                    break;
                case ICmpInst::SGT: // a > b -> b < a -> slt rd, b, a
                    curMCBlk->push((new MCInst(MCInst::SLT))->add(rd)->add(rhs)->add(lhs));
                    break;
                case ICmpInst::SLE: // a <= b -> !(b < a) -> slt rd, b, a; xori rd, rd, 1
                    curMCBlk->push((new MCInst(MCInst::SLT))->add(rd)->add(rhs)->add(lhs));
                    curMCBlk->push((new MCInst(MCInst::XORI))->add(rd)->add(rd)->add(MCOpnd::imm(1)));
                    break;
                case ICmpInst::SGE: // a >= b -> !(a < b) -> slt rd, a, b; xori rd, rd, 1
                    curMCBlk->push((new MCInst(MCInst::SLT))->add(rd)->add(lhs)->add(rhs));
                    curMCBlk->push((new MCInst(MCInst::XORI))->add(rd)->add(rd)->add(MCOpnd::imm(1)));
                    break;
            }
            break;
        }
        case Instruction::FCmp: {
            auto cmp = cast<FCmpInst>(inst);
            MCOpnd rd = getOpnd(inst);
            MCOpnd lhs = getOpnd(cmp->getOperand(0));
            MCOpnd rhs = getOpnd(cmp->getOperand(1));

            switch(cmp->getPredicate()) {
                case FCmpInst::OEQ: // a == b -> feq.s rd, a, b
                    curMCBlk->push((new MCInst(MCInst::FEQ_S))->add(rd)->add(lhs)->add(rhs));
                    break;
                case FCmpInst::ONE: // a != b -> feq.s t, a, b; xori rd, t, 1
                    {
                        MCOpnd tmp = createVReg();
                        curMCBlk->push((new MCInst(MCInst::FEQ_S))->add(tmp)->add(lhs)->add(rhs));
                        curMCBlk->push((new MCInst(MCInst::XORI))->add(rd)->add(tmp)->add(MCOpnd::imm(1)));
                    }
                    break;
                case FCmpInst::OLT: // a < b -> flt.s rd, a, b
                    curMCBlk->push((new MCInst(MCInst::FLT_S))->add(rd)->add(lhs)->add(rhs));
                    break;
                case FCmpInst::OGT: // a > b -> b < a -> flt.s rd, b, a
                    curMCBlk->push((new MCInst(MCInst::FLT_S))->add(rd)->add(rhs)->add(lhs));
                    break;
                case FCmpInst::OLE: // a <= b -> fle.s rd, a, b
                    curMCBlk->push((new MCInst(MCInst::FLE_S))->add(rd)->add(lhs)->add(rhs));
                    break;
                case FCmpInst::OGE: // a >= b -> b <= a -> fle.s rd, b, a
                    curMCBlk->push((new MCInst(MCInst::FLE_S))->add(rd)->add(rhs)->add(lhs));
                    break;
                }
            break;
        }
        case Instruction::Br: {
            auto brInst = cast<BranchInst>(inst);
            if (brInst->getNumOperands() == 1) { 
                // J target
                BasicBlock* target = cast<BasicBlock>(brInst->getOperand(0));
                curMCBlk->push((new MCInst(MCInst::J))->add(MCOpnd::lbl(bbMap[target]->name)));
            } else { 
                // BNE cond, zero, true_bb; J false_bb
                MCOpnd cond = getOpnd(brInst->getOperand(0));
                BasicBlock* trueBB = cast<BasicBlock>(brInst->getOperand(1));
                BasicBlock* falseBB = cast<BasicBlock>(brInst->getOperand(2));
                
                curMCBlk->push((new MCInst(MCInst::BNE))->add(cond)->add(MCOpnd::preg(PReg::zero))->add(MCOpnd::lbl(bbMap[trueBB]->name)));
                curMCBlk->push((new MCInst(MCInst::J))->add(MCOpnd::lbl(bbMap[falseBB]->name)));
            }
            break;
        }
        case Instruction::GetElementPtr: {
            // GEP = base + index * 4
            MCOpnd rd = getOpnd(inst);
            MCOpnd base = getOpnd(inst->getOperand(0));
            MCOpnd idx = getOpnd(inst->getOperand(1));
            
            MCOpnd offset = createVReg();
            curMCBlk->push((new MCInst(MCInst::SLLI))->add(offset)->add(idx)->add(MCOpnd::imm(2)));
            curMCBlk->push((new MCInst(MCInst::ADD))->add(rd)->add(base)->add(offset));
            break;
        }
        case Instruction::Load: {
            Type* ty = inst->getType();
            
            // As long as there is a pointer or array property,
            // it is forced to be downgraded to an integer load. 
            bool isF = ty->isFloat() && !ty->isPointer() && !ty->isArray();
            
            curMCBlk->push((new MCInst(isF ? MCInst::FLW : MCInst::LW))
                           ->add(getOpnd(inst))
                           ->add(getOpnd(inst->getOperand(0)))
                           ->add(MCOpnd::imm(0)));
            break;
        }
        case Instruction::Store: {
            Type* ty = inst->getOperand(0)->getType();

            bool isF = ty->isFloat() && !ty->isPointer() && !ty->isArray();
            
            curMCBlk->push((new MCInst(isF ? MCInst::FSW : MCInst::SW))
                           ->add(getOpnd(inst->getOperand(0)))
                           ->add(getOpnd(inst->getOperand(1)))
                           ->add(MCOpnd::imm(0)));
            break;
        }
        case Instruction::Alloca: {
            // The local variable space is given to subsequent stack frame allocation
            auto allocaInst = cast<AllocaInst>(inst);
            Type* allocTy = allocaInst->getAllocatedType();

            int size = getTypeSize(allocTy);

            curMCBlk->push((new MCInst(MCInst::ALLOCA))->add(getOpnd(inst))->add(MCOpnd::imm(size)));
            break;
        }
        case Instruction::Call: {
            auto callInst = cast<CallInst>(inst);
            int intCnt = 0;
            int floatCnt = 0;
            Function* callee = callInst->getFunction();

            for (int i = 1; i < callInst->getNumOperands(); i++) {
                MCOpnd arg = getOpnd(callInst->getOperand(i));
                Type* argType = callInst->getOperand(i)->getType();

                bool isFloatArg = argType->isFloat() && !argType->isPointer() && !argType->isArray();

                bool expectsFloat = false;
                if (i - 1 < callee->getArgs().size()) {
                    Type* expectType = callee->getArgs()[i - 1]->getType();
                    expectsFloat = expectType->isFloat() && !expectType->isPointer() && !expectType->isArray();
                } else {
                    expectsFloat = isFloatArg;
                }

                PReg targetReg = expectsFloat ? 
                    static_cast<PReg>(static_cast<int>(PReg::fa0) + floatCnt++) : 
                    static_cast<PReg>(static_cast<int>(PReg::a0) + intCnt++);

                if (isFloatArg && !expectsFloat) {
                    curMCBlk->push((new MCInst(MCInst::FMV_X_W))->add(MCOpnd::preg(targetReg))->add(arg));
                } else if (!isFloatArg && expectsFloat) {
                    curMCBlk->push((new MCInst(MCInst::FMV_W_X))->add(MCOpnd::preg(targetReg))->add(arg));
                } else if (isFloatArg && expectsFloat) {
                    curMCBlk->push((new MCInst(MCInst::FMV_S))->add(MCOpnd::preg(targetReg))->add(arg));
                } else {
                    curMCBlk->push((new MCInst(MCInst::MV))->add(MCOpnd::preg(targetReg))->add(arg));
                }
            }
            
            curMCBlk->push((new MCInst(MCInst::CALL))->add(MCOpnd::lbl(callInst->getFunction()->getName())));
            
            if (!callInst->getType()->isVoid()) {
                if (callInst->getType()->isFloat()) {
                    curMCBlk->push((new MCInst(MCInst::FMV_S))->add(getOpnd(callInst))->add(MCOpnd::preg(PReg::fa0)));
                } else {
                    curMCBlk->push((new MCInst(MCInst::MV))->add(getOpnd(callInst))->add(MCOpnd::preg(PReg::a0)));
                }
            }
            break;
        }
        case Instruction::Ret: {
            if (inst->getNumOperands() > 0) {
                // Using a0/fa0 as the return value register.
                curMCBlk->push((new MCInst(MCInst::MV))->add(MCOpnd::preg(PReg::a0))->add(getOpnd(inst->getOperand(0))));
            }
            curMCBlk->push((new MCInst(MCInst::RET)));
            break;
        }
        case Instruction::Phi: {
            auto phiInst = cast<PhiInst>(inst);
            auto mcPhi = new MCInst(MCInst::PHI);
            mcPhi->add(getOpnd(phiInst));
            
            for (int i = 0; i < phiInst->getNumOperands(); i += 2) {
                MCOpnd val = getOpnd(phiInst->getOperand(i));
                BasicBlock* fromBB = cast<BasicBlock>(phiInst->getOperand(i+1));
                mcPhi->add(val)->add(MCOpnd::lbl(bbMap[fromBB]->name));
            }
            curMCBlk->push(mcPhi);
            break;
        }
        default:
            std::cerr << "Warning: Unhandled instruction in selection!" << std::endl;
            std::cerr << inst->getOpID() << std::endl;
            break;
    }
}