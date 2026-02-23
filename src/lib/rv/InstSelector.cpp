#include "rv/InstSelector.h"
#include "IR/Instruction.h"
#include <iostream>

using namespace sysy;

static int getTypeSize(Type* ty) {
    if (auto arrTy = dyn_cast<ArrayType>(ty)) {
        return arrTy->getNumElements() * getTypeSize(arrTy->getElementType());
    }
    if (ty->isPointer()) return 8;
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
        curMCBlk->func->blks.front()->push_front(liInst);
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
        // Then the bits in the integer register are moved to the floating-point register.
        // FMV_W_X floatReg, intReg
        auto fmvInst = new MCInst(MCInst::FMV_W_X);
        fmvInst->add(floatReg)->add(intReg);

        curMCBlk->func->blks.front()->push_front(fmvInst);
        curMCBlk->func->blks.front()->push_front(liInst);
        val2opnd[val] = floatReg;

        return floatReg;
    }

    if (auto gv = dyn_cast<GlobalVariable>(val)) {
        MCOpnd vreg = createVReg();
        // Using LA to load the label of the global variable.
        auto laInst = new MCInst(MCInst::LA);
        laInst->add(vreg)->add(MCOpnd::lbl(gv->getName()));

        curMCBlk->func->blks.front()->push_front(laInst);
        val2opnd[val] = vreg;

        return vreg;
    }

    // such as function parameters, first allocate a virtual register as a fallback
    MCOpnd vreg = createVReg();
    val2opnd[val] = vreg;
    return vreg;
}

void InstSelector::selectFunction(Function* func) {
    // Each function has an independent virtual register number, starting at 0.
    nextVRegNo = 0;
    val2opnd.clear();
    bbMap.clear();

    curMCFunc = new MCFunc(func->getName());
    curMCMod->add(curMCFunc);

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
        // Handle division and modulo, intercept and optimize powers of 2.
        case Instruction::Div:
        case Instruction::Mod: {
            MCOpnd rd = getOpnd(inst);
            MCOpnd lhs = getOpnd(inst->getOperand(0));
            Value* rhsVal = inst->getOperand(1);

            int constVal = 0;
            bool isConst = false;
            if (auto c = dyn_cast<ConstantInt>(rhsVal)) {
                constVal = c->getValue();
                isConst = true;
            } else if (auto load = dyn_cast<LoadInst>(rhsVal)) {
                if (auto gv = dyn_cast<GlobalVariable>(load->getOperand(0))) {
                    if (auto init = dyn_cast<ConstantInt>(gv->getInit())) {
                        constVal = init->getValue();
                        isConst = true;
                    }
                }
            }

            if (isConst && constVal == 16) {
                MCOpnd sign = createVReg();
                MCOpnd mask = createVReg();
                MCOpnd adjusted = createVReg();
                
                curMCBlk->push((new MCInst(MCInst::SRAIW))->add(sign)->add(lhs)->add(MCOpnd::imm(31)));
                curMCBlk->push((new MCInst(MCInst::SRLIW))->add(mask)->add(sign)->add(MCOpnd::imm(28)));
                curMCBlk->push((new MCInst(MCInst::ADDW))->add(adjusted)->add(lhs)->add(mask));
                
                if (inst->getOpID() == Instruction::Div) {
                    curMCBlk->push((new MCInst(MCInst::SRAIW))->add(rd)->add(adjusted)->add(MCOpnd::imm(4)));
                } else {
                    MCOpnd divRes = createVReg();
                    MCOpnd mulRes = createVReg();
                    curMCBlk->push((new MCInst(MCInst::SRAIW))->add(divRes)->add(adjusted)->add(MCOpnd::imm(4)));
                    curMCBlk->push((new MCInst(MCInst::SLLIW))->add(mulRes)->add(divRes)->add(MCOpnd::imm(4)));
                    curMCBlk->push((new MCInst(MCInst::SUBW))->add(rd)->add(lhs)->add(mulRes));
                }
            } else {                
                MCOpnd rhsReg = getOpnd(rhsVal);
                if (rhsReg.isImm()) {
                    MCOpnd temp = createVReg();
                    curMCBlk->push((new MCInst(MCInst::LI))->add(temp)->add(rhsReg));
                    rhsReg = temp;
                }
                if (inst->getOpID() == Instruction::Div) {
                    curMCBlk->push((new MCInst(MCInst::DIVW))->add(rd)->add(lhs)->add(rhsReg));
                } else {
                    curMCBlk->push((new MCInst(MCInst::REMW))->add(rd)->add(lhs)->add(rhsReg));
                }
            }
            break;
        }
        case Instruction::Add:
        case Instruction::Sub:
        case Instruction::Mul:
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
            
            Type* baseType = inst->getOperand(0)->getType();
            int stride = 4;
            if (auto ptrTy = dyn_cast<PointerType>(baseType)) {
                Value* baseVal = inst->getOperand(0);
                bool isPointer = isa<Argument>(baseVal) || isa<LoadInst>(baseVal);
                
                Type* pointeeTy = ptrTy->getPointeeType();
                if (isPointer) {
                    stride = getTypeSize(pointeeTy);
                } else {
                    if (auto arrTy = dyn_cast<ArrayType>(pointeeTy)) {
                        stride = getTypeSize(arrTy->getElementType());
                    } else {
                        stride = getTypeSize(pointeeTy);
                    }
                }
            }

            if (stride == 4) {
                // 1D array / base pointer: Directly optimized SLLI.
                MCOpnd offset = createVReg();
                curMCBlk->push((new MCInst(MCInst::SLLI))->add(offset)->add(idx)->add(MCOpnd::imm(2)));
                curMCBlk->push((new MCInst(MCInst::ADD))->add(rd)->add(base)->add(offset));
            } else {
                // Multidimensional matrix: Check if the step size is a power of 2 (e.g. 1024*4 = 4096).
                bool isPowerOf2 = stride > 0 && (stride & (stride - 1)) == 0;
                if (isPowerOf2) {
                    int shift = 0;
                    int temp = stride;
                    while (temp > 1) { temp >>= 1; shift++; }
                    MCOpnd offset = createVReg();
                    curMCBlk->push((new MCInst(MCInst::SLLI))->add(offset)->add(idx)->add(MCOpnd::imm(shift)));
                    curMCBlk->push((new MCInst(MCInst::ADD))->add(rd)->add(base)->add(offset));
                } else {
                    // If it's not a power of 2, execute the multiplication instruction honestly.
                    MCOpnd strideReg = createVReg();
                    MCOpnd offset = createVReg();
                    curMCBlk->push((new MCInst(MCInst::LI))->add(strideReg)->add(MCOpnd::imm(stride)));
                    curMCBlk->push((new MCInst(MCInst::MULW))->add(offset)->add(idx)->add(strideReg));
                    curMCBlk->push((new MCInst(MCInst::ADD))->add(rd)->add(base)->add(offset));
                }
            }
            break;
        }
        case Instruction::Load: {
            Type* ty = inst->getType();
            
            // As long as there is a pointer or array property,
            // it is forced to be downgraded to an integer load. 
            bool isF = ty->isFloat() && !ty->isPointer() && !ty->isArray();
            bool isPtr = ty->isPointer() || ty->isArray();
            
            MCInst::Opc opc = isPtr ? MCInst::LD : (isF ? MCInst::FLW : MCInst::LW);
            
            curMCBlk->push((new MCInst(opc))
                           ->add(getOpnd(inst))
                           ->add(getOpnd(inst->getOperand(0)))
                           ->add(MCOpnd::imm(0)));
            break;
        }
        case Instruction::Store: {
            Type* ty = inst->getOperand(0)->getType();

            bool isF = ty->isFloat() && !ty->isPointer() && !ty->isArray();
            bool isPtr = ty->isPointer() || ty->isArray();
            
            MCInst::Opc opc = isPtr ? MCInst::SD : (isF ? MCInst::FSW : MCInst::SW);
            
            curMCBlk->push((new MCInst(opc))
                           ->add(getOpnd(inst->getOperand(0)))
                           ->add(getOpnd(inst->getOperand(1)))
                           ->add(MCOpnd::imm(0)));
            break;
        }
        case Instruction::Alloca: {
            // The local variable space is given to subsequent stack frame allocation
            auto allocaInst = cast<AllocaInst>(inst);
            Type* allocTy = allocaInst->getAllocatedType();

            if (auto ptrTy = dyn_cast<PointerType>(allocTy)) {
                allocTy = ptrTy->getPointeeType();
            }

            std::function<int(Type*)> getSz = [&](Type* t) -> int {
                if (t->isArray()) {
                    auto arrT = dyn_cast<ArrayType>(t);
                    return arrT->getNumElements() * getSz(arrT->getElementType());
                }
                return 4;
            };

            int size = getSz(allocTy);

            size = (size + 7) / 8 * 8; // Align to 8 bytes

            curMCBlk->push((new MCInst(MCInst::ALLOCA))->add(getOpnd(inst))->add(MCOpnd::imm(size)));
            break;
        }
        case Instruction::Call: {
            auto callInst = cast<CallInst>(inst);
            auto mcCall = new MCInst(MCInst::CALL);
            mcCall->add(MCOpnd::lbl(callInst->getOperand(0)->getName()));

            for (size_t i = 1; i < callInst->getNumOperands(); ++i) {
                Value* argVal = callInst->getOperand(i);
                bool isF = argVal->getType()->isFloat();
                MCOpnd argVR = createVReg();
                
                PReg targetReg = isF ? static_cast<PReg>(static_cast<int>(PReg::fa0) + i - 1) 
                                     : static_cast<PReg>(static_cast<int>(PReg::a0) + i - 1);
                
                curMCFunc->precolorMap[argVR.val] = targetReg;
                curMCBlk->push((new MCInst(isF ? MCInst::FMV_S : MCInst::MV))->add(argVR)->add(getOpnd(argVal)));
                
                mcCall->add(argVR);
            }
            curMCBlk->push(mcCall);

            if (!callInst->getType()->isVoid()) {
                bool isF = callInst->getType()->isFloat();
                curMCBlk->push((new MCInst(isF ? MCInst::FMV_S : MCInst::MV))->add(getOpnd(callInst))->add(MCOpnd::preg(isF ? PReg::fa0 : PReg::a0)));
            }
            break;
        }
        case Instruction::Ret: {
            auto mcRet = new MCInst(MCInst::RET);
            if (inst->getNumOperands() > 0) {
                Value* retVal = inst->getOperand(0);
                bool isF = retVal->getType()->isFloat();
                MCOpnd retVR = createVReg();

                curMCFunc->precolorMap[retVR.val] = isF ? PReg::fa0 : PReg::a0;
                curMCBlk->push((new MCInst(isF ? MCInst::FMV_S : MCInst::MV))->add(retVR)->add(getOpnd(retVal)));
                
                mcRet->add(retVR);
            }
            curMCBlk->push(mcRet);
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