#include "rv/InstSel.h"
#include "IR/Module.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cassert>

namespace sysy {
namespace rv {

MCOperand InstSelContext::getVReg(Value* v, bool isFloat) {
    auto it = valueMap.find(v);
    if (it != valueMap.end()) return it->second;

    // If the current block already has a termination instruction, insert it before; otherwise, append.
    // Prevents constant materialization instructions from falling after j/bnez during Phi digestion (dead instructions).
    auto insertSafe = [&](RvOp* op) {
        RvOp* term = block->getTerminator();
        if (term) block->insertBefore(term, op);
        else block->append(op);
    };

    if (auto* ci = dyn_cast<ConstantInt>(v)) {
        // Fold integer zero to the hardware zero register, no li needed.
        if (ci->getValue() == 0) return MCOperand(Reg::zero);
        auto cit = constMap.find(v);
        if (cit != constMap.end()) return cit->second;
        auto vreg = newVReg(false);
        insertSafe(new LiOp(vreg, ci->getValue()));
        constMap[v] = vreg;
        return vreg;
    }

    if (auto* cf = dyn_cast<ConstantFloat>(v)) {
        if (cf->getValue() == 0.0f) {
            if (isFloat) {
                auto cit = constMap.find(v);
                if (cit != constMap.end()) return cit->second;
                auto vreg = newVReg(true);
                insertSafe(new FMvWXOp(vreg, MCOperand(Reg::zero)));
                constMap[v] = vreg;
                return vreg;
            } else {
                return MCOperand(Reg::zero);
            }
        }
    }

    if (isa<ConstantZero>(v) && !isFloat) {
        return MCOperand(Reg::zero);
    }

    // ConstantZero (float)：fmv.w.x fd, zero → 0.0f
    if (isa<ConstantZero>(v) && isFloat) {
        auto cit = constMap.find(v);
        if (cit != constMap.end()) return cit->second;
        auto vreg = newVReg(true);
        insertSafe(new FMvWXOp(vreg, MCOperand(Reg::zero)));
        constMap[v] = vreg;
        return vreg;
    }

    // bits -> intVReg -> floatVReg
    if (auto* cf = dyn_cast<ConstantFloat>(v)) {
        auto cit = constMap.find(v);
        if (cit != constMap.end()) return cit->second;
        float fval = cf->getValue();
        int bits;
        memcpy(&bits, &fval, sizeof(int));
        auto tmpInt = newVReg(false);
        auto vreg   = newVReg(true);
        insertSafe(new LiOp(tmpInt, bits));
        insertSafe(new FMvWXOp(vreg, tmpInt));
        constMap[v] = vreg;
        return vreg;
    }

    // GlobalVariable
    if (auto* gv = dyn_cast<GlobalVariable>(v)) {
        auto cit = constMap.find(v);
        if (cit != constMap.end()) return cit->second;
        auto vreg = newVReg(false);
        insertSafe(new LaOp(vreg, gv->getName()));
        constMap[v] = vreg;
        return vreg;
    }

    auto vreg = newVReg(isFloat);
    valueMap[v] = vreg;
    return vreg;
}

MCOperand InstSelContext::newVReg(bool isFloat) {
    return MCOperand(func->newVReg(isFloat));
}

std::vector<std::unique_ptr<MCFunction>> InstSelPass::run(Module* module) {
    std::vector<std::unique_ptr<MCFunction>> functions;
    for (auto* irFunc : module->getFunctions()) {
        auto* mcFunc = selectFunction(irFunc);
        if (mcFunc) {
            functions.push_back(std::unique_ptr<MCFunction>(mcFunc));
        }
    }

    return functions;
}

MCFunction* InstSelPass::selectFunction(Function* irFunc) {
    // External declaration (no body) — skip, nothing to select.
    if (irFunc->getBody()->getBlocks().empty()) return nullptr;

    auto* func = new MCFunction(irFunc->getName());

    InstSelContext ctx;
    ctx.func = func;

    // Handle arguments
    {
        int intIdx = 0, floatIdx = 0, stackSlot = 0;
        for (auto* arg : irFunc->getArgs()) {
            bool isFloat = InstSelContext::isFloatType(arg->getType());
            bool isPtr = arg->getType()->isPointer() || arg->getType()->isArray();
            auto vreg = ctx.newVReg(isFloat);
            ctx.valueMap[arg] = vreg;
            if (isPtr) {
                VReg v = vreg.getVReg();
                if (v < func->vregInfo.size()) func->vregInfo[v].isPtr = true;
            }

            if (isFloat && floatIdx < 8) {
                func->args.push_back(vreg.getVReg());
                func->argIsFloat.push_back(true);
                floatIdx++;
            } else if (!isFloat && intIdx < 8) {
                func->args.push_back(vreg.getVReg());
                func->argIsFloat.push_back(false);
                intIdx++;
            } else {
                func->incomingStackArgs.push_back({vreg.getVReg(), stackSlot++, isFloat});
            }
        }
    }

    for (auto* bb : irFunc->getBody()->getBlocks()) {
        selectBasicBlock(bb, ctx);
    }

    for (size_t i = 0; i < func->blocks.size(); ++i) {
        func->blocks[i]->index = static_cast<int>(i);
    }

    // Build CFG edges: Scan the jump instructions in each block.
    // Terminate is either jump or branch.
    for (auto& mcBB : func->blocks) {
        mcBB->forEach([&](RvOp* op) {
            std::string target;
            if (op->opcode == RvOp::JOp) {
                target = static_cast<JOp*>(op)->label;
            } else if (op->opcode >= RvOp::BeqOp && op->opcode <= RvOp::BgtzOp) {
                target = static_cast<RVInstB*>(op)->target;
            } else {
                return;
            }
            for (auto& b : func->blocks) {
                if (b->name == target) {
                    mcBB->addSucc(b.get());
                    break;
                }
            }
        });
    }

    // Phi Elimination & Critical Edge Splitting
    // Insert Jump Block "
    //
    // src_to_dst: 
    // mv ...; 
    // j dst;
    //
    // " 
    // and redirected bnez to the springboard to fix the critical edge issue
    for (auto* irBB : irFunc->getBody()->getBlocks()) {
        MCBlock* dstMCBlock = nullptr;
        for (auto& mb : func->blocks) {
            if (mb->name == func->name + "." + irBB->getName()) { dstMCBlock = mb.get(); break; }
        }
        if (!dstMCBlock) continue;

        for (auto* inst : irBB->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) continue;

            bool isFloat = InstSelContext::isFloatType(phi->getType());
            MCOperand dstReg = ctx.getVReg(phi, isFloat);

            // PhiInst operands: [val0, bb0, val1, bb1, ...]
            for (int i = 0; i + 1 < phi->getNumOperands(); i += 2) {
                Value* incomingVal = phi->getOperand(i);
                auto* incomingBB = static_cast<BasicBlock*>(phi->getOperand(i + 1));

                MCBlock* srcMCBlock = nullptr;
                for (auto& mb : func->blocks) {
                    if (mb->name == func->name + "." + incomingBB->getName()) { srcMCBlock = mb.get(); break; }
                }
                if (!srcMCBlock) continue;

                // Determine the insertion block before calling getVReg.
                std::string trampolineName = srcMCBlock->name + "_to_" + dstMCBlock->name;
                MCBlock* trampoline = nullptr;
                for (auto& mb : func->blocks) {
                    if (mb->name == trampolineName) { trampoline = mb.get(); break; }
                }

                if (!trampoline) {
                    RvOp* term0 = srcMCBlock->getTerminator();
                    RvOp* branchOp = nullptr;
                    if (term0 && term0->opcode == RvOp::JOp) {
                        for (RvOp* op = term0->prev; op != nullptr; op = op->prev) {
                            if (op->opcode >= RvOp::BeqOp && op->opcode <= RvOp::BgtzOp &&
                                static_cast<RVInstB*>(op)->target == dstMCBlock->name) {
                                branchOp = op;
                                break;
                            }
                            // Stop if we hit another terminator-like op.
                            if (op->opcode == RvOp::JOp || op->opcode == RvOp::RetOp) break;
                        }
                    }

                    if (branchOp) {
                        // Create the trampoline now, before any getVReg calls.
                        trampoline = func->createBlock(trampolineName);
                        trampoline->index = static_cast<int>(func->blocks.size() - 1);
                        trampoline->append(new JOp(dstMCBlock->name));
                        static_cast<RVInstB*>(branchOp)->target = trampolineName;

                        if (static_cast<JOp*>(term0)->label == dstMCBlock->name) {
                            static_cast<JOp*>(term0)->label = trampolineName;
                        }

                        auto itSrc = std::find(srcMCBlock->succs.begin(), srcMCBlock->succs.end(), dstMCBlock);
                        if (itSrc != srcMCBlock->succs.end())
                            srcMCBlock->succs.erase(itSrc);
                        
                        auto itDst = std::find(dstMCBlock->preds.begin(), dstMCBlock->preds.end(), srcMCBlock);
                        if (itDst != dstMCBlock->preds.end())
                            dstMCBlock->preds.erase(itDst);

                        srcMCBlock->addSucc(trampoline);
                        trampoline->addSucc(dstMCBlock);
                    }
                }

                // Direct ctx.block to the insertion block so getVReg inserts there.
                MCBlock* insertBlock = trampoline ? trampoline : srcMCBlock;
                ctx.block = insertBlock;

                ctx.constMap.clear();
                ctx.scaledIndexCache.clear();

                // Build mvOp
                RvOp* mvOp = nullptr;
                if (!isFloat) {
                    if (auto* ci = dyn_cast<ConstantInt>(incomingVal))
                        mvOp = new LiOp(dstReg, ci->getValue());
                    else if (isa<ConstantZero>(incomingVal))
                        mvOp = new MvOp(dstReg, MCOperand(Reg::zero));
                } else if (isa<ConstantZero>(incomingVal)) {
                    mvOp = new FMvWXOp(dstReg, MCOperand(Reg::zero));
                }

                if (!mvOp) {
                    MCOperand srcReg = ctx.getVReg(incomingVal, isFloat);
                    if (dstReg == srcReg) continue;
                    mvOp = isFloat
                        ? static_cast<RvOp*>(new FMvSOp(dstReg, srcReg))
                        : static_cast<RvOp*>(new MvOp(dstReg, srcReg));
                }

                // Insert mvOp into the correct block before its terminator.
                RvOp* tTerm = insertBlock->getTerminator();
                if (tTerm) insertBlock->insertBefore(tTerm, mvOp);
                else insertBlock->append(mvOp);
            }
        }
    }

    // Check if the func has CallOp.
    func->analyzeLeaf();
    func->buildDefUseChains();

    return func;
}

void InstSelPass::selectBasicBlock(BasicBlock* irBB, InstSelContext& ctx) {
    auto* block = ctx.func->createBlock(ctx.func->name + "." + irBB->getName());
    ctx.block = block;
    ctx.scaledIndexCache.clear();
    ctx.constMap.clear();

    for (auto* inst : irBB->getInstructions()) {
        selectInstruction(inst, ctx);
    }
}

void InstSelPass::selectInstruction(Instruction* inst, InstSelContext& ctx) {
    switch (inst->getOpID()) {
        case Instruction::Add:
            selectAdd(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::Sub:
            selectSub(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::Mul:
            selectMul(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::Div:
            selectDiv(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::Mod:
            selectMod(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::FAdd:
            selectFAdd(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::FSub:
            selectFSub(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::FMul:
            selectFMul(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::FDiv:
            selectFDiv(static_cast<BinaryInst*>(inst), ctx);
            break;
        case Instruction::Alloca:
            selectAlloca(static_cast<AllocaInst*>(inst), ctx);
            break;
        case Instruction::Load:
            selectLoad(static_cast<LoadInst*>(inst), ctx);
            break;
        case Instruction::Store:
            selectStore(static_cast<StoreInst*>(inst), ctx);
            break;
        case Instruction::GetElementPtr:
            selectGetElementPtr(static_cast<GetElementPtrInst*>(inst), ctx);
            break;
        case Instruction::SIToFP:
            selectSIToFP(static_cast<CastInst*>(inst), ctx);
            break;
        case Instruction::FPToSI:
            selectFPToSI(static_cast<CastInst*>(inst), ctx);
            break;
        case Instruction::ICmp:
            selectICmp(static_cast<ICmpInst*>(inst), ctx);
            break;
        case Instruction::FCmp:
            selectFCmp(static_cast<FCmpInst*>(inst), ctx);
            break;
        case Instruction::Br:
            selectBranch(static_cast<BranchInst*>(inst), ctx);
            break;
        case Instruction::Ret:
            selectReturn(static_cast<ReturnInst*>(inst), ctx);
            break;
        case Instruction::Call:
            selectCall(static_cast<CallInst*>(inst), ctx);
            break;
        case Instruction::If:
            selectIf(static_cast<IfInst*>(inst), ctx);
            break;
        case Instruction::While:
            selectWhile(static_cast<WhileInst*>(inst), ctx);
            break;
        case Instruction::Break:
            selectBreak(static_cast<BreakInst*>(inst), ctx);
            break;
        case Instruction::Continue:
            selectContinue(static_cast<ContinueInst*>(inst), ctx);
            break;

        // Phi
        case Instruction::Phi:
            selectPhi(static_cast<PhiInst*>(inst), ctx);
            break;

        default:
            std::cerr << "Unknown instruction opcode: " << inst->getOpID() << std::endl;
            break;
    }
}

void InstSelPass::selectAdd(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);

    // Check if have one operand is constant, which can use addi.
    auto tryAddi = [&](Value* base, Value* imm) -> bool {
        auto* ci = dyn_cast<ConstantInt>(imm);
        if (!ci) return false;
        int v = ci->getValue();
        if (v < -2048 || v > 2047) return false;
        auto rs = ctx.getVReg(base, false);
        ctx.block->append(new AddiOp(rd, rs, v));
        return true;
    };

    if (tryAddi(lhs, rhs) || tryAddi(rhs, lhs)) return;

    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new AddwOp(rd, rs1, rs2));
}

void InstSelPass::selectSub(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);

    // sub x, c  ->  addi rd, x, -c  
    // when -c fits in 12-bit signed immediate.
    if (auto* ci = dyn_cast<ConstantInt>(rhs)) {
        int neg = -ci->getValue();
        if (neg >= -2048 && neg <= 2047) {
            auto rs1 = ctx.getVReg(lhs, false);
            ctx.block->append(new AddiOp(rd, rs1, neg));
            return;
        }
    }

    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new SubwOp(rd, rs1, rs2));
}

void InstSelPass::selectMul(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);

    // Strength reduction: x * 2^k  ->  slliw rd, x, k
    auto tryShift = [&](Value* base, Value* imm) -> bool {
        auto* ci = dyn_cast<ConstantInt>(imm);
        if (!ci || ci->getValue() <= 0) return false;
        int v = ci->getValue();
        if (v & (v - 1)) return false;  // not a power of 2
        // __builtin_ctz counts trailing zeros.
        int k = __builtin_ctz(static_cast<unsigned>(v));
        auto rs = ctx.getVReg(base, false);
        ctx.block->append(new SlliwOp(rd, rs, k));
        return true;
    };
    if (tryShift(lhs, rhs) || tryShift(rhs, lhs)) return;

    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new MulwOp(rd, rs1, rs2));
}

void InstSelPass::selectDiv(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);

    // Cannot directly use right shift to replace div,
    // because -3 >> 1 == -2, but -3 / 2 == -1.
    // So we need to add Bias 2^k - 1 for x < 0 case.
    // (x + (x<0 ? 2^k - 1 : 0)) >> k
    if (auto* ci = dyn_cast<ConstantInt>(rhs)) {
        int v = ci->getValue();
        if (v > 1 && (v & (v - 1)) == 0) {
            int k = __builtin_ctz(static_cast<unsigned>(v));
            auto rs = ctx.getVReg(lhs, false);
            // adj = (x >> 31) >> (32-k); (x + adj) >> k
            auto sign = ctx.newVReg(false);
            ctx.block->append(new SraiwOp(sign, rs, 31));
            auto adj = ctx.newVReg(false);
            ctx.block->append(new SrliwOp(adj, sign, 32 - k));
            auto xadj = ctx.newVReg(false);
            // rs + adj
            ctx.block->append(new AddwOp(xadj, rs, adj));
            ctx.block->append(new SraiwOp(rd, xadj, k));
            return;
        }
    }

    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new DivwOp(rd, rs1, rs2));
}

void InstSelPass::selectMod(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd  = ctx.getVReg(inst, false);

    // adj = (x>>31) >> (32-k);  
    // t = (x+adj) & (2^k-1);  
    // t - adj
    if (auto* ci = dyn_cast<ConstantInt>(rhs)) {
        int v = ci->getValue();
        if (v > 1 && (v & (v - 1)) == 0) {
            int k = __builtin_ctz(static_cast<unsigned>(v));
            int mask = v - 1;
            auto rs = ctx.getVReg(lhs, false);
            // adj = (rs >> 31) >> (32 - k)
            auto sign = ctx.newVReg(false);
            ctx.block->append(new SraiwOp(sign, rs, 31));
            auto adj = ctx.newVReg(false);
            ctx.block->append(new SrliwOp(adj, sign, 32 - k));
            // rs + adj
            auto xadj = ctx.newVReg(false);
            ctx.block->append(new AddwOp(xadj, rs, adj));
            auto masked = ctx.newVReg(false);
            if (mask <= 2047) {
                ctx.block->append(new AndiOp(masked, xadj, mask));
            } else {
                // mask too large for 12-bit imm: use li + and
                auto mReg = ctx.newVReg(false);
                ctx.block->append(new LiOp(mReg, mask));
                ctx.block->append(new AndOp(masked, xadj, mReg));
            }
            // masked - adj
            ctx.block->append(new SubwOp(rd, masked, adj));
            return;
        }
    }

    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new RemwOp(rd, rs1, rs2));
}

void InstSelPass::selectFAdd(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto fd = ctx.getVReg(inst, true);
    auto fs1 = ctx.getVReg(lhs, true);
    auto fs2 = ctx.getVReg(rhs, true);
    ctx.block->append(new FAddSOp(fd, fs1, fs2));
}

void InstSelPass::selectFSub(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto fd = ctx.getVReg(inst, true);
    auto fs1 = ctx.getVReg(lhs, true);
    auto fs2 = ctx.getVReg(rhs, true);
    ctx.block->append(new FSubSOp(fd, fs1, fs2));
}

void InstSelPass::selectFMul(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto fd = ctx.getVReg(inst, true);
    auto fs1 = ctx.getVReg(lhs, true);
    auto fs2 = ctx.getVReg(rhs, true);
    ctx.block->append(new FMulSOp(fd, fs1, fs2));
}

void InstSelPass::selectFDiv(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto fd = ctx.getVReg(inst, true);
    auto fs1 = ctx.getVReg(lhs, true);
    auto fs2 = ctx.getVReg(rhs, true);
    ctx.block->append(new FDivSOp(fd, fs1, fs2));
}

static int typeByteSize(Type* ty) {
    if (ty->isArray()) {
        auto* arr = static_cast<ArrayType*>(ty);
        return arr->getNumElements() * typeByteSize(arr->getElementType());
    }
    return 4; // int, float
}

void InstSelPass::selectAlloca(AllocaInst* inst, InstSelContext& ctx) {
    // Result is a pointer.
    auto vreg = ctx.getVReg(inst, false);
    VReg v = vreg.getVReg();
    if (v < ctx.func->vregInfo.size())
        ctx.func->vregInfo[v].isPtr = true;

    if (ctx.func->allocaLocalOffset.count(v)) return;

    if (!inst->getAllocatedType()) return;
    int bytes = typeByteSize(inst->getAllocatedType());
    bytes = (bytes + 7) & ~7;

    int localOff = ctx.func->allocaSize;
    ctx.func->allocaSize += bytes;
    ctx.func->allocaLocalOffset[v] = localOff;

    // MC Alloca Hoisting, insert placeholder in entrybb.
    MCBlock* entryBlock = ctx.func->blocks.front().get();
    auto* placeholder = new AddiOp(vreg, MCOperand(Reg::sp), 0);
    RvOp* term = entryBlock->getTerminator();
    if (term) entryBlock->insertBefore(term, placeholder);
    else entryBlock->append(placeholder);
    ctx.func->allocaAddrInsts.push_back({v, placeholder});
}

void InstSelPass::selectLoad(LoadInst* inst, InstSelContext& ctx) {
    auto* ptr = inst->getOperand(0);
    bool isFloat = InstSelContext::isFloatType(inst->getType());
    bool isPtr = inst->getType()->isPointer();

    auto rd = ctx.getVReg(inst, isFloat);
    if (isPtr && rd.isVReg()) {
        VReg rv = rd.getVReg();
        if (rv < ctx.func->vregInfo.size()) ctx.func->vregInfo[rv].isPtr = true;
    }
    auto base = ctx.getVReg(ptr, false);

    if (isFloat) {
        ctx.block->append(new FLwOp(rd, base, 0));
    } else if (isPtr) {
        ctx.block->append(new LdOp(rd, base, 0));
    } else {
        ctx.block->append(new LwOp(rd, base, 0));
    }
}

void InstSelPass::selectStore(StoreInst* inst, InstSelContext& ctx) {
    auto* val = inst->getOperand(0);
    auto* ptr = inst->getOperand(1);
    bool isFloat = InstSelContext::isFloatType(val->getType());
    bool isPtr = val->getType()->isPointer();

    auto src = ctx.getVReg(val, isFloat);
    auto base = ctx.getVReg(ptr, false);

    if (isFloat) {
        ctx.block->append(new FSwOp(src, base, 0));
    } else if (isPtr) {
        ctx.block->append(new SdOp(src, base, 0));
    } else {
        ctx.block->append(new SwOp(src, base, 0));
    }
}

void InstSelPass::selectGetElementPtr(GetElementPtrInst* inst, InstSelContext& ctx) {
    auto* base  = inst->getOperand(0);
    auto* index = inst->getOperand(1);

    auto baseReg = ctx.getVReg(base, false);
    if (baseReg.isVReg()) {
        VReg bv = baseReg.getVReg();
        if (bv < ctx.func->vregInfo.size()) ctx.func->vregInfo[bv].isPtr = true;
    }

    auto elemType = static_cast<PointerType*>(base->getType())->getPointeeType();
    int elemSize = typeByteSize(elemType);

    // Constant index: compile-time computation offset, eliminating runtime multiplication.
    if (auto* ci = dyn_cast<ConstantInt>(index)) {
        int offset = ci->getValue() * elemSize;

        if (offset == 0) {
            auto existing_it = ctx.valueMap.find(inst);
            if (existing_it != ctx.valueMap.end()) {
                if (existing_it->second != baseReg) {
                    auto* mvOp = new MvOp(existing_it->second, baseReg);
                    auto* term = ctx.block->getTerminator();
                    if (term) ctx.block->insertBefore(term, mvOp);
                    else ctx.block->append(mvOp);
                }
            } else {
                ctx.valueMap[inst] = baseReg;
            }
            return;
        }

        auto rd = ctx.getVReg(inst, false);
        if (rd.isVReg()) {
            VReg rv = rd.getVReg();
            if (rv < ctx.func->vregInfo.size()) ctx.func->vregInfo[rv].isPtr = true;
        }

        // Offset within 12-bit signed range: an addi.
        if (offset >= -2048 && offset <= 2047) {
            ctx.block->append(new AddiOp(rd, baseReg, offset));
            return;
        }

        // Large offset: li + add (64-bit, pointer arithmetic must not truncate).
        auto tmp = ctx.newVReg(false);
        ctx.block->append(new LiOp(tmp, offset));
        ctx.block->append(new AddOp(rd, baseReg, tmp));
        return;
    }

    auto rd = ctx.getVReg(inst, false);
    if (rd.isVReg()) {
        VReg rv = rd.getVReg();
        if (rv < ctx.func->vregInfo.size()) ctx.func->vregInfo[rv].isPtr = true;
    }
    auto indexReg = ctx.getVReg(index, false);

    if (elemSize == 1) {
        ctx.block->append(new AddOp(rd, baseReg, indexReg)); // bool
    } else {
        MCOperand tmp2;
        if (indexReg.isVReg()) {
            auto cacheKey = std::make_pair(indexReg.getVReg(), elemSize);
            auto it = ctx.scaledIndexCache.find(cacheKey);
            if (it != ctx.scaledIndexCache.end()) {
                tmp2 = it->second;
            } else {
                tmp2 = ctx.newVReg(false);
                if ((elemSize & (elemSize - 1)) == 0) {
                    int k = __builtin_ctz(static_cast<unsigned>(elemSize));
                    ctx.block->append(new SlliwOp(tmp2, indexReg, k));
                } else {
                    auto tmp1 = ctx.newVReg(false);
                    ctx.block->append(new LiOp(tmp1, elemSize));
                    ctx.block->append(new MulwOp(tmp2, indexReg, tmp1));
                }
                ctx.scaledIndexCache[cacheKey] = tmp2;
            }
        } else {
            // Physical register: no caching, generate directly.
            tmp2 = ctx.newVReg(false);
            if ((elemSize & (elemSize - 1)) == 0) {
                int k = __builtin_ctz(static_cast<unsigned>(elemSize));
                ctx.block->append(new SlliwOp(tmp2, indexReg, k));
            } else {
                auto tmp1 = ctx.newVReg(false);
                ctx.block->append(new LiOp(tmp1, elemSize));
                ctx.block->append(new MulwOp(tmp2, indexReg, tmp1));
            }
        }
        ctx.block->append(new AddOp(rd, baseReg, tmp2));
    }
}

void InstSelPass::selectSIToFP(CastInst* inst, InstSelContext& ctx) {
    auto* val = inst->getOperand(0);
    auto fd = ctx.getVReg(inst, true);
    auto rs = ctx.getVReg(val, false);
    ctx.block->append(new FCvtSWOp(fd, rs));
}

void InstSelPass::selectFPToSI(CastInst* inst, InstSelContext& ctx) {
    auto* val = inst->getOperand(0);
    auto rd = ctx.getVReg(inst, false);
    auto fs = ctx.getVReg(val, true);
    ctx.block->append(new FCvtWSOp(rd, fs));
}

void InstSelPass::selectICmp(ICmpInst* inst, InstSelContext& ctx) {
    // Check if the comparison is used only by a branch.
    // If so, we can skip.
    {
        const auto& users = inst->getUsers();
        bool onlyBranch = !users.empty();
        for (auto* u : users) {
            if (!isa<BranchInst>(u)) { onlyBranch = false; break; }
        }
        if (onlyBranch) return;
    }

    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto pred = inst->getPredicate();

    auto rd = ctx.getVReg(inst, false);

    // Detect integer zero without materializing a li rd, 0.
    auto isIntZero = [](Value* v) -> bool {
        if (isa<ConstantZero>(v)) return true;
        if (auto* ci = dyn_cast<ConstantInt>(v)) return ci->getValue() == 0;
        return false;
    };

    // SLT with small non-zero constant rhs: slti rd, rs, imm  (1 instruction).
    if (pred == ICmpInst::SLT) {
        if (auto* ci = dyn_cast<ConstantInt>(rhs)) {
            int v = ci->getValue();
            if (v != 0 && v >= -2048 && v <= 2047) {
                auto rs = ctx.getVReg(lhs, false);
                ctx.block->append(new SltiOp(rd, rs, v));
                return;
            }
        }
    }

    // SLT/SGT with zero: fold zero operand into the zero register, saving a li.
    //   icmp slt x, 0  ->  slt rd, x, zero
    //   icmp slt 0, x  ->  slt rd, zero, x
    //   icmp sgt x, 0  ->  slt rd, zero, x   (reversed operands)
    //   icmp sgt 0, x  ->  slt rd, x, zero
    if ((pred == ICmpInst::SLT || pred == ICmpInst::SGT) &&
        (isIntZero(lhs) || isIntZero(rhs))) {
        bool isSgt = (pred == ICmpInst::SGT);
        // For SGT(a,b) = SLT(b,a), swap lhs/rhs.
        Value* sltLhs = isSgt ? rhs : lhs;
        Value* sltRhs = isSgt ? lhs : rhs;
        auto r1 = isIntZero(sltLhs) ? MCOperand(Reg::zero) : ctx.getVReg(sltLhs, false);
        auto r2 = isIntZero(sltRhs) ? MCOperand(Reg::zero) : ctx.getVReg(sltRhs, false);
        ctx.block->append(new SltOp(rd, r1, r2));
        return;
    }

    // EQ/NE with zero: use seqz / snez (single instruction).
    if (pred == ICmpInst::EQ && (isIntZero(lhs) || isIntZero(rhs))) {
        // seqz rd, rs  =>  sltiu rd, rs, 1
        auto rs = ctx.getVReg(isIntZero(rhs) ? lhs : rhs, false);
        ctx.block->append(new SltiuOp(rd, rs, 1));
        return;
    }
    if (pred == ICmpInst::NE && (isIntZero(lhs) || isIntZero(rhs))) {
        // snez rd, rs  =>  sltu rd, zero, rs
        auto rs = ctx.getVReg(isIntZero(rhs) ? lhs : rhs, false);
        ctx.block->append(new SltuOp(rd, MCOperand(Reg::zero), rs));
        return;
    }

    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);

    switch (pred) {
        case ICmpInst::EQ: {
            // xor tmp, rs1, rs2  -> tmp==0 if rs1==rs2
            // sltiu rd, tmp, 1   -> rd = (tmp == 0)
            auto tmp = ctx.newVReg(false);
            ctx.block->append(new XorOp(tmp, rs1, rs2));
            ctx.block->append(new SltiuOp(rd, tmp, 1));
            break;
        }
        case ICmpInst::NE: {
            auto lt = ctx.newVReg(false);
            ctx.block->append(new SltOp(lt, rs1, rs2));
            auto gt = ctx.newVReg(false);
            ctx.block->append(new SltOp(gt, rs2, rs1));
            ctx.block->append(new OrOp(rd, lt, gt));
            break;
        }
        case ICmpInst::SLT:
            ctx.block->append(new SltOp(rd, rs1, rs2));
            break;
        case ICmpInst::SGT:
            ctx.block->append(new SltOp(rd, rs2, rs1));
            break;
        case ICmpInst::SLE:
        case ICmpInst::SGE: {
            auto tmp = ctx.newVReg(false);
            bool le = (pred == ICmpInst::SLE);
            ctx.block->append(new SltOp(tmp, le ? rs2 : rs1, le ? rs1 : rs2));
            ctx.block->append(new XoriOp(rd, tmp, 1));
            break;
        }
    }
}

void InstSelPass::selectFCmp(FCmpInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto pred = inst->getPredicate();

    auto rd = ctx.getVReg(inst, false);
    auto fs1 = ctx.getVReg(lhs, true);
    auto fs2 = ctx.getVReg(rhs, true);

    switch (pred) {
        case FCmpInst::OEQ:
            ctx.block->append(new FEQSOp(rd, fs1, fs2));
            break;
        case FCmpInst::ONE: {
            auto tmp = ctx.newVReg(false);
            ctx.block->append(new FEQSOp(tmp, fs1, fs2));
            ctx.block->append(new XoriOp(rd, tmp, 1));
            break;
        }
        case FCmpInst::OLT:
            ctx.block->append(new FLTSOp(rd, fs1, fs2));
            break;
        case FCmpInst::OGT:
            ctx.block->append(new FLTSOp(rd, fs2, fs1));
            break;
        case FCmpInst::OLE:
        case FCmpInst::OGE: {
            auto tmp = ctx.newVReg(false);
            bool le = (pred == FCmpInst::OLE);
            ctx.block->append(new FLTSOp(tmp, le ? fs2 : fs1, le ? fs1 : fs2));
            ctx.block->append(new XoriOp(rd, tmp, 1));
            break;
        }
    }
}

void InstSelPass::selectBranch(BranchInst* inst, InstSelContext& ctx) {
    if (inst->getNumOperands() == 1) {
        // j
        auto* dest = static_cast<BasicBlock*>(inst->getOperand(0));
        ctx.block->append(new JOp(ctx.func->name + "." + dest->getName()));
    } else {
        // branch
        auto* cond = inst->getOperand(0);
        auto* ifTrue = static_cast<BasicBlock*>(inst->getOperand(1));
        auto* ifFalse = static_cast<BasicBlock*>(inst->getOperand(2));
        std::string trueLabel  = ctx.func->name + "." + ifTrue->getName();
        std::string falseLabel = ctx.func->name + "." + ifFalse->getName();

        if (auto* icmp = dyn_cast<ICmpInst>(cond)) {
            auto* lhs = icmp->getOperand(0);
            auto* rhs = icmp->getOperand(1);
            auto pred = icmp->getPredicate();

            auto rs1 = ctx.getVReg(lhs, false);
            auto rs2 = ctx.getVReg(rhs, false);
            switch (pred) {
                case ICmpInst::EQ:  ctx.block->append(new BeqOp(rs1, rs2, trueLabel)); break;
                case ICmpInst::NE:  ctx.block->append(new BneOp(rs1, rs2, trueLabel)); break;
                case ICmpInst::SLT: ctx.block->append(new BltOp(rs1, rs2, trueLabel)); break;
                case ICmpInst::SGT: ctx.block->append(new BltOp(rs2, rs1, trueLabel)); break;
                case ICmpInst::SLE: ctx.block->append(new BgeOp(rs2, rs1, trueLabel)); break;
                case ICmpInst::SGE: ctx.block->append(new BgeOp(rs1, rs2, trueLabel)); break;
                default: ctx.block->append(new BnezOp(ctx.getVReg(cond, false), trueLabel)); break;
            }
            ctx.block->append(new JOp(falseLabel));
            return;
        }
        auto condReg = ctx.getVReg(cond, false);
        ctx.block->append(new BnezOp(condReg, trueLabel));
        ctx.block->append(new JOp(falseLabel));
    }
}

// Using a0/fa0 as return register.
void InstSelPass::selectReturn(ReturnInst* inst, InstSelContext& ctx) {
    if (inst->getNumOperands() > 0) {
        auto* val = inst->getOperand(0);
        bool isFloat = InstSelContext::isFloatType(val->getType());

        // Constant return value: materialize directly into a0/fa0,
        // avoiding an intermediate vreg + mv.
        if (!isFloat) {
            if (auto* ci = dyn_cast<ConstantInt>(val)) {
                ctx.block->append(new LiOp(MCOperand(Reg::a0), ci->getValue()));
                ctx.block->append(new RetOp());
                return;
            }
            if (isa<ConstantZero>(val)) {
                ctx.block->append(new MvOp(MCOperand(Reg::a0), MCOperand(Reg::zero)));
                ctx.block->append(new RetOp());
                return;
            }
        }

        auto valReg = ctx.getVReg(val, isFloat);
        if (isFloat)
            ctx.block->append(new FMvSOp(MCOperand(Reg::fa0), valReg));
        else
            ctx.block->append(new MvOp(MCOperand(Reg::a0), valReg));
    }
    ctx.block->append(new RetOp());
}

void InstSelPass::selectCall(CallInst* inst, InstSelContext& ctx) {
    auto* callee = inst->getFunction();
    auto* callOp = new CallOp(callee->getName());

    int numOps = inst->getNumOperands();

    std::vector<MCOperand> argVRegs(numOps);
    {
        int intIdx = 0, floatIdx = 0;
        for (int i = 1; i < numOps; ++i) {
            auto* arg = inst->getOperand(i);
            bool isFloat = InstSelContext::isFloatType(arg->getType());

            bool isRegArg = (isFloat && floatIdx < 8) || (!isFloat && intIdx < 8);
            if (isFloat && floatIdx < 8) floatIdx++;
            else if (!isFloat && intIdx < 8) intIdx++;

            // Constant int/zero register args: leave argVRegs[i] empty;
            if (isRegArg && !isFloat && (isa<ConstantInt>(arg) || isa<ConstantZero>(arg)))
                continue;

            argVRegs[i] = ctx.getVReg(arg, isFloat);
        }
    }

    {
        int intIdx = 0, floatIdx = 0, stackSlot = 0;
        for (int i = 1; i < numOps; ++i) {
            auto* arg = inst->getOperand(i);
            bool isFloat = InstSelContext::isFloatType(arg->getType());

            if (isFloat && floatIdx < 8) {
                Reg paramReg = static_cast<Reg>(static_cast<int>(Reg::fa0) + floatIdx++);
                ctx.block->append(new FMvSOp(MCOperand(paramReg), argVRegs[i]));
            } else if (!isFloat && intIdx < 8) {
                Reg paramReg = static_cast<Reg>(static_cast<int>(Reg::a0) + intIdx++);
                if (auto* ci = dyn_cast<ConstantInt>(arg))
                    // li aX, imm
                    ctx.block->append(new LiOp(MCOperand(paramReg), ci->getValue()));
                else if (isa<ConstantZero>(arg))
                    // mv aX, zero
                    ctx.block->append(new MvOp(MCOperand(paramReg), MCOperand(Reg::zero)));
                else
                    // mv aX, vreg
                    ctx.block->append(new MvOp(MCOperand(paramReg), argVRegs[i]));
            } else {
                callOp->stackArgs.push_back({argVRegs[i], stackSlot++, isFloat});
            }
        }
    }

    ctx.block->append(callOp);

    if (!inst->getType()->isVoid()) {
        bool isFloat = InstSelContext::isFloatType(inst->getType());
        if (isFloat) {
            auto rd = ctx.getVReg(inst, true);
            ctx.block->append(new FMvSOp(rd, MCOperand(Reg::fa0)));
        } else {
            auto rd = ctx.getVReg(inst, false);
            ctx.block->append(new MvOp(rd, MCOperand(Reg::a0)));
        }
    }
}

void InstSelPass::selectPhi(PhiInst* inst, InstSelContext& ctx) {
    // After Early Phi Elimination, here is only valuemap.
    // PhiInst is not corresponding to MCInst.
    ctx.getVReg(inst, InstSelContext::isFloatType(inst->getType()));
}

// Through FlattenCFG, If/While/Break/Continue have all been expanded into BranchInst.
void InstSelPass::selectIf(IfInst*, InstSelContext&) {
    assert(false && "IfInst reached InstSel: FlattenCFG should have lowered it");
}

void InstSelPass::selectWhile(WhileInst*, InstSelContext&) {
    assert(false && "WhileInst reached InstSel: FlattenCFG should have lowered it");
}

void InstSelPass::selectBreak(BreakInst*, InstSelContext&) {
    assert(false && "BreakInst reached InstSel: FlattenCFG should have lowered it");
}

void InstSelPass::selectContinue(ContinueInst*, InstSelContext&) {
    assert(false && "ContinueInst reached InstSel: FlattenCFG should have lowered it");
}

} // namespace rv
} // namespace sysy
