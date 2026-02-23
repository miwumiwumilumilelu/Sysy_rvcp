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
        curMCBlk->func->blks.front()->push_front(liInst);
        val2opnd[val] = vreg;
        return vreg;
    }

    if (auto cf = dyn_cast<ConstantFloat>(val)) {
        float fval = cf->getValue();
        int imm = *reinterpret_cast<int*>(&fval);
        MCOpnd intReg = createVReg();
        MCOpnd floatReg = createVReg();

        auto liInst = new MCInst(MCInst::LI);
        liInst->add(intReg)->add(MCOpnd::imm(imm));
        auto fmvInst = new MCInst(MCInst::FMV_W_X);
        fmvInst->add(floatReg)->add(intReg);

        // 必须按顺序 push_front，fmv 在前，li 在后，这样生成的顺序才是 li 先执行！
        curMCBlk->func->blks.front()->push_front(fmvInst);
        curMCBlk->func->blks.front()->push_front(liInst);
        val2opnd[val] = floatReg;

        return floatReg;
    }

    if (auto gv = dyn_cast<GlobalVariable>(val)) {
        MCOpnd vreg = createVReg();
        auto laInst = new MCInst(MCInst::LA);
        laInst->add(vreg)->add(MCOpnd::lbl(gv->getName()));

        curMCBlk->func->blks.front()->push_front(laInst);
        val2opnd[val] = vreg;

        return vreg;
    }

    MCOpnd vreg = createVReg();
    val2opnd[val] = vreg;
    return vreg;
}

void InstSelector::selectFunction(Function* func) {
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
            if (floatCnt < 8) {
                PReg preg = static_cast<PReg>(static_cast<int>(PReg::fa0) + floatCnt++);
                curMCBlk->push((new MCInst(MCInst::FMV_S))->add(vreg)->add(MCOpnd::preg(preg)));
            } else if (floatCnt < 14) { 
                static PReg extFloatRegs[] = {PReg::ft1, PReg::ft2, PReg::ft3, PReg::ft4, PReg::ft5, PReg::ft6};
                curMCBlk->push((new MCInst(MCInst::FMV_S))->add(vreg)->add(MCOpnd::preg(extFloatRegs[floatCnt++ - 8])));
            }
        } else {
            if (intCnt < 8) {
                PReg preg = static_cast<PReg>(static_cast<int>(PReg::a0) + intCnt++);
                curMCBlk->push((new MCInst(MCInst::MV))->add(vreg)->add(MCOpnd::preg(preg)));
            } else if (intCnt < 14) { // for manyargs
                static PReg extIntRegs[] = {PReg::t1, PReg::t2, PReg::t3, PReg::t4, PReg::t5, PReg::t6};
                curMCBlk->push((new MCInst(MCInst::MV))->add(vreg)->add(MCOpnd::preg(extIntRegs[intCnt++ - 8])));
            }
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
                case ICmpInst::EQ: 
                    curMCBlk->push((new MCInst(MCInst::XOR))->add(rd)->add(lhs)->add(rhs));
                    curMCBlk->push((new MCInst(MCInst::SEQZ))->add(rd)->add(rd));
                    break;
                case ICmpInst::NE: 
                    curMCBlk->push((new MCInst(MCInst::XOR))->add(rd)->add(lhs)->add(rhs));
                    curMCBlk->push((new MCInst(MCInst::SNEZ))->add(rd)->add(rd));
                    break;
                case ICmpInst::SLT: 
                    curMCBlk->push((new MCInst(MCInst::SLT))->add(rd)->add(lhs)->add(rhs));
                    break;
                case ICmpInst::SGT: 
                    curMCBlk->push((new MCInst(MCInst::SLT))->add(rd)->add(rhs)->add(lhs));
                    break;
                case ICmpInst::SLE: 
                    curMCBlk->push((new MCInst(MCInst::SLT))->add(rd)->add(rhs)->add(lhs));
                    curMCBlk->push((new MCInst(MCInst::XORI))->add(rd)->add(rd)->add(MCOpnd::imm(1)));
                    break;
                case ICmpInst::SGE: 
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
                case FCmpInst::OEQ: 
                    curMCBlk->push((new MCInst(MCInst::FEQ_S))->add(rd)->add(lhs)->add(rhs));
                    break;
                case FCmpInst::ONE: 
                    {
                        MCOpnd tmp = createVReg();
                        curMCBlk->push((new MCInst(MCInst::FEQ_S))->add(tmp)->add(lhs)->add(rhs));
                        curMCBlk->push((new MCInst(MCInst::XORI))->add(rd)->add(tmp)->add(MCOpnd::imm(1)));
                    }
                    break;
                case FCmpInst::OLT: 
                    curMCBlk->push((new MCInst(MCInst::FLT_S))->add(rd)->add(lhs)->add(rhs));
                    break;
                case FCmpInst::OGT: 
                    curMCBlk->push((new MCInst(MCInst::FLT_S))->add(rd)->add(rhs)->add(lhs));
                    break;
                case FCmpInst::OLE: 
                    curMCBlk->push((new MCInst(MCInst::FLE_S))->add(rd)->add(lhs)->add(rhs));
                    break;
                case FCmpInst::OGE: 
                    curMCBlk->push((new MCInst(MCInst::FLE_S))->add(rd)->add(rhs)->add(lhs));
                    break;
                }
            break;
        }
        case Instruction::Br: {
            auto brInst = cast<BranchInst>(inst);
            if (brInst->getNumOperands() == 1) { 
                BasicBlock* target = cast<BasicBlock>(brInst->getOperand(0));
                curMCBlk->push((new MCInst(MCInst::J))->add(MCOpnd::lbl(bbMap[target]->name)));
            } else { 
                MCOpnd cond = getOpnd(brInst->getOperand(0));
                BasicBlock* trueBB = cast<BasicBlock>(brInst->getOperand(1));
                BasicBlock* falseBB = cast<BasicBlock>(brInst->getOperand(2));
                
                curMCBlk->push((new MCInst(MCInst::BNE))->add(cond)->add(MCOpnd::preg(PReg::zero))->add(MCOpnd::lbl(bbMap[trueBB]->name)));
                curMCBlk->push((new MCInst(MCInst::J))->add(MCOpnd::lbl(bbMap[falseBB]->name)));
            }
            break;
        }
        case Instruction::GetElementPtr: {
            MCOpnd rd = getOpnd(inst);
            MCOpnd base = getOpnd(inst->getOperand(0));
            MCOpnd idx = getOpnd(inst->getOperand(1));
            
            Type* baseType = inst->getOperand(0)->getType();
            Type* pointeeTy = cast<PointerType>(baseType)->getPointeeType();
            
            int stride = getTypeSize(pointeeTy);

            MCOpnd strideReg = createVReg();
            MCOpnd offset = createVReg();
            curMCBlk->push((new MCInst(MCInst::LI))->add(strideReg)->add(MCOpnd::imm(stride)));
            curMCBlk->push((new MCInst(MCInst::MULW))->add(offset)->add(idx)->add(strideReg));
            curMCBlk->push((new MCInst(MCInst::ADD))->add(rd)->add(base)->add(offset));
            break;
        }
        case Instruction::Load: {
            Type* ty = inst->getType();
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
            auto allocaInst = cast<AllocaInst>(inst);
            Type* allocTy = allocaInst->getAllocatedType();

            if (auto ptrTy = dyn_cast<PointerType>(allocTy)) allocTy = ptrTy->getPointeeType();

            std::function<int(Type*)> getSz = [&](Type* t) -> int {
                if (t->isArray()) {
                    auto arrT = dyn_cast<ArrayType>(t);
                    return arrT->getNumElements() * getSz(arrT->getElementType());
                }
                return 4;
            };

            int size = getSz(allocTy);
            size = (size + 7) / 8 * 8; 

            curMCBlk->push((new MCInst(MCInst::ALLOCA))->add(getOpnd(inst))->add(MCOpnd::imm(size)));
            break;
        }
        case Instruction::Call: {
            auto callInst = cast<CallInst>(inst);
            auto mcCall = new MCInst(MCInst::CALL);
            mcCall->add(MCOpnd::lbl(callInst->getOperand(0)->getName()));

            int intCnt = 0;
            int floatCnt = 0;
            for (size_t i = 1; i < callInst->getNumOperands(); ++i) {
                Value* argVal = callInst->getOperand(i);
                bool isF = argVal->getType()->isFloat();
                MCOpnd argVR = createVReg();
                
                PReg targetReg = PReg::zero;
                if (isF) {
                    if (floatCnt < 8) {
                        targetReg = static_cast<PReg>(static_cast<int>(PReg::fa0) + floatCnt++);
                    } else if (floatCnt < 14) { 
                        static PReg extFloatRegs[] = {PReg::ft1, PReg::ft2, PReg::ft3, PReg::ft4, PReg::ft5, PReg::ft6};
                        targetReg = extFloatRegs[floatCnt++ - 8];
                    }
                } else {
                    if (intCnt < 8) {
                        targetReg = static_cast<PReg>(static_cast<int>(PReg::a0) + intCnt++);
                    } else if (intCnt < 14) { 
                        static PReg extIntRegs[] = {PReg::t1, PReg::t2, PReg::t3, PReg::t4, PReg::t5, PReg::t6};
                        targetReg = extIntRegs[intCnt++ - 8];
                    }
                }
                
                if (targetReg != PReg::zero) {
                    curMCFunc->precolorMap[argVR.val] = targetReg;
                    curMCBlk->push((new MCInst(isF ? MCInst::FMV_S : MCInst::MV))->add(argVR)->add(getOpnd(argVal)));
                    mcCall->add(argVR);
                }
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
            break;
    }
}