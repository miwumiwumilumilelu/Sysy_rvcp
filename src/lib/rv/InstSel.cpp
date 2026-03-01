#include "rv/InstSel.h"
#include "IR/Module.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include <iostream>

namespace sysy {
namespace rv {

// ============================================================================
// InstSelContext 实现
// ============================================================================

MCOperand InstSelContext::getVReg(Value* v, bool isFloat) {
    auto it = valueMap.find(v);
    if (it != valueMap.end()) {
        return it->second;
    }
    // 创建新的 VReg
    auto vreg = newVReg(isFloat);
    valueMap[v] = vreg;
    return vreg;
}

MCOperand InstSelContext::newVReg(bool isFloat) {
    return MCOperand(func->newVReg(isFloat));
}

// ============================================================================
// InstSelPass 实现
// ============================================================================

std::vector<std::unique_ptr<MCFunction>> InstSelPass::run(Module* module) {
    std::vector<std::unique_ptr<MCFunction>> functions;

    // 遍历模块中的所有函数
    for (auto* irFunc : module->getFunctions()) {
        auto* mcFunc = selectFunction(irFunc);
        if (mcFunc) {
            functions.push_back(std::unique_ptr<MCFunction>(mcFunc));
        }
    }

    return functions;
}

MCFunction* InstSelPass::selectFunction(Function* irFunc) {
    // 创建 MCFunction
    auto* func = new MCFunction(irFunc->getName());

    InstSelContext ctx;
    ctx.func = func;

    // 处理函数参数
    for (auto* arg : irFunc->getArgs()) {
        bool isFloat = InstSelContext::isFloatType(arg->getType());
        auto vreg = ctx.newVReg(isFloat);
        ctx.valueMap[arg] = vreg;
        func->args.push_back(vreg.getVReg());
        func->argIsFloat.push_back(isFloat);
    }

    // 遍历所有基本块
    auto* body = irFunc->getBody();
    for (auto* bb : body->getBlocks()) {
        selectBasicBlock(bb, ctx);
    }

    // 分析是否为叶子函数
    func->analyzeLeaf();

    // 构建 Def-Use 链
    func->buildDefUseChains();

    return func;
}

void InstSelPass::selectBasicBlock(BasicBlock* irBB, InstSelContext& ctx) {
    // 创建 MCBlock
    auto* block = ctx.func->createBlock(irBB->getName());
    ctx.block = block;

    // 遍历基本块中的所有指令
    for (auto* inst : irBB->getInstructions()) {
        selectInstruction(inst, ctx);
    }
}

void InstSelPass::selectInstruction(Instruction* inst, InstSelContext& ctx) {
    switch (inst->getOpID()) {
    // 算术运算
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

    // 浮点运算
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

    // 内存访问
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

    // 类型转换
    case Instruction::SIToFP:
        selectSIToFP(static_cast<CastInst*>(inst), ctx);
        break;
    case Instruction::FPToSI:
        selectFPToSI(static_cast<CastInst*>(inst), ctx);
        break;

    // 比较
    case Instruction::ICmp:
        selectICmp(static_cast<ICmpInst*>(inst), ctx);
        break;
    case Instruction::FCmp:
        selectFCmp(static_cast<FCmpInst*>(inst), ctx);
        break;

    // 控制流
    case Instruction::Br:
        selectBranch(static_cast<BranchInst*>(inst), ctx);
        break;
    case Instruction::Ret:
        selectReturn(static_cast<ReturnInst*>(inst), ctx);
        break;
    case Instruction::Call:
        selectCall(static_cast<CallInst*>(inst), ctx);
        break;

    // 高级控制流
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

// ============================================================================
// 算术运算
// ============================================================================

void InstSelPass::selectAdd(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);
    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new AddwOp(rd, rs1, rs2));
}

void InstSelPass::selectSub(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);
    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new SubwOp(rd, rs1, rs2));
}

void InstSelPass::selectMul(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);
    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new MulwOp(rd, rs1, rs2));
}

void InstSelPass::selectDiv(BinaryInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto rd = ctx.getVReg(inst, false);
    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);
    ctx.block->append(new DivwOp(rd, rs1, rs2));
}

void InstSelPass::selectMod(BinaryInst* inst, InstSelContext& ctx) {
    // a % b = a - (a / b) * b
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);

    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);

    // 临时寄存器
    auto quot = ctx.newVReg(false);
    auto mulRes = ctx.newVReg(false);
    auto rd = ctx.getVReg(inst, false);

    // quot = a / b
    ctx.block->append(new DivwOp(quot, rs1, rs2));
    // mulRes = quot * b
    ctx.block->append(new MulwOp(mulRes, quot, rs2));
    // rd = a - mulRes
    ctx.block->append(new SubwOp(rd, rs1, mulRes));
}

// ============================================================================
// 浮点运算
// ============================================================================

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

// ============================================================================
// 内存访问
// ============================================================================

void InstSelPass::selectAlloca(AllocaInst* inst, InstSelContext& ctx) {
    // Alloca 需要被转换为栈偏移
    // 这里暂时创建一个虚拟寄存器，后续在栈帧布局时处理
    auto vreg = ctx.getVReg(inst, false);
    // TODO: 计算栈偏移并存储到 ctx.func 的栈帧信息中
}

void InstSelPass::selectLoad(LoadInst* inst, InstSelContext& ctx) {
    auto* ptr = inst->getOperand(0);
    bool isFloat = InstSelContext::isFloatType(inst->getType());

    auto rd = ctx.getVReg(inst, isFloat);
    auto base = ctx.getVReg(ptr, false);

    // 暂时使用偏移 0，GEP 会计算实际偏移
    if (isFloat) {
        ctx.block->append(new FLwOp(rd, base, 0));
    } else {
        ctx.block->append(new LwOp(rd, base, 0));
    }
}

void InstSelPass::selectStore(StoreInst* inst, InstSelContext& ctx) {
    auto* val = inst->getOperand(0);
    auto* ptr = inst->getOperand(1);
    bool isFloat = InstSelContext::isFloatType(val->getType());

    auto src = ctx.getVReg(val, isFloat);
    auto base = ctx.getVReg(ptr, false);

    if (isFloat) {
        ctx.block->append(new FSwOp(src, base, 0));
    } else {
        ctx.block->append(new SwOp(src, base, 0));
    }
}

void InstSelPass::selectGetElementPtr(GetElementPtrInst* inst, InstSelContext& ctx) {
    // GEP: base + index * element_size
    auto* base = inst->getOperand(0);
    auto* index = inst->getOperand(1);

    auto rd = ctx.getVReg(inst, false);
    auto baseReg = ctx.getVReg(base, false);
    auto indexReg = ctx.getVReg(index, false);

    // 计算元素大小（简化处理，假设 i32 = 4 字节）
    auto elemType = static_cast<PointerType*>(base->getType())->getPointeeType();
    int elemSize = 4;  // 默认 4 字节
    if (elemType->isArray()) {
        elemSize = 4 * static_cast<ArrayType*>(elemType)->getNumElements();
    }

    if (elemSize == 1) {
        ctx.block->append(new AddwOp(rd, baseReg, indexReg));
    } else {
        auto tmp = ctx.newVReg(false);
        ctx.block->append(new LiOp(tmp, elemSize));
        ctx.block->append(new MulwOp(tmp, indexReg, tmp));
        ctx.block->append(new AddwOp(rd, baseReg, tmp));
    }
}

// ============================================================================
// 类型转换
// ============================================================================

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

// ============================================================================
// 比较
// ============================================================================

void InstSelPass::selectICmp(ICmpInst* inst, InstSelContext& ctx) {
    auto* lhs = inst->getOperand(0);
    auto* rhs = inst->getOperand(1);
    auto pred = inst->getPredicate();

    auto rd = ctx.getVReg(inst, false);
    auto rs1 = ctx.getVReg(lhs, false);
    auto rs2 = ctx.getVReg(rhs, false);

    // 使用 slt 实现
    switch (pred) {
    case ICmpInst::EQ: {
        // a == b → !(a - b) < 0 且 !(a - b) > 0
        // 使用 sltu 实现：a == b → (a+b) 无符号溢出检查
        auto tmp = ctx.newVReg(false);
        ctx.block->append(new XorOp(tmp, rs1, rs2));
        ctx.block->append(new SltuOp(rd, tmp, MCOperand(1)));
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
        auto one = ctx.newVReg(false);
        ctx.block->append(new LiOp(one, 1));
        ctx.block->append(new XorOp(rd, tmp, one));
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
        auto one = ctx.newVReg(false);
        ctx.block->append(new LiOp(one, 1));
        ctx.block->append(new XorOp(rd, tmp, one));
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
        auto one = ctx.newVReg(false);
        ctx.block->append(new LiOp(one, 1));
        ctx.block->append(new XorOp(rd, tmp, one));
        break;
    }
    }
}

// ============================================================================
// 控制流
// ============================================================================

void InstSelPass::selectBranch(BranchInst* inst, InstSelContext& ctx) {
    // 检查是否为无条件跳转（根据 Instruction.h 的接口）
    // 如果只有一个操作数，则为无条件跳转到目标
    // 如果有两个操作数，则为条件跳转

    if (inst->getNumOperands() == 1) {
        // 无条件跳转
        auto* dest = static_cast<BasicBlock*>(inst->getOperand(0));
        ctx.block->append(new JOp(dest->getName()));
    } else {
        // 条件跳转
        auto* cond = inst->getOperand(0);
        auto* ifTrue = static_cast<BasicBlock*>(inst->getOperand(1));
        auto* ifFalse = static_cast<BasicBlock*>(inst->getOperand(2));

        auto condReg = ctx.getVReg(cond, false);
        ctx.block->append(new BnezOp(condReg, ifTrue->getName()));
        ctx.block->append(new JOp(ifFalse->getName()));
    }
}

void InstSelPass::selectReturn(ReturnInst* inst, InstSelContext& ctx) {
    if (inst->getNumOperands() > 0) {
        auto* val = inst->getOperand(0);
        bool isFloat = InstSelContext::isFloatType(val->getType());
        auto valReg = ctx.getVReg(val, isFloat);

        if (isFloat) {
            ctx.block->append(new MvOp(MCOperand(Reg::fa0), valReg));
        } else {
            ctx.block->append(new MvOp(MCOperand(Reg::a0), valReg));
        }
    }
    ctx.block->append(new RetOp());
}

void InstSelPass::selectCall(CallInst* inst, InstSelContext& ctx) {
    auto* callee = inst->getFunction();

    // 将参数移动到参数寄存器
    int intIdx = 0;
    int floatIdx = 0;
    for (int i = 0; i < inst->getNumOperands(); ++i) {
        auto* arg = inst->getOperand(i);
        bool isFloat = InstSelContext::isFloatType(arg->getType());
        auto argReg = ctx.getVReg(arg, isFloat);

        if (isFloat && floatIdx < 8) {
            Reg paramReg = static_cast<Reg>(static_cast<int>(Reg::fa0) + floatIdx);
            ctx.block->append(new MvOp(MCOperand(paramReg), argReg));
            floatIdx++;
        } else if (!isFloat && intIdx < 8) {
            Reg paramReg = static_cast<Reg>(static_cast<int>(Reg::a0) + intIdx);
            ctx.block->append(new MvOp(MCOperand(paramReg), argReg));
            intIdx++;
        }
    }

    ctx.block->append(new CallOp(callee->getName()));

    // 处理返回值
    if (!inst->getType()->isVoid()) {
        bool isFloat = InstSelContext::isFloatType(inst->getType());
        if (isFloat) {
            auto rd = ctx.getVReg(inst, true);
            ctx.block->append(new MvOp(rd, MCOperand(Reg::fa0)));
        } else {
            auto rd = ctx.getVReg(inst, false);
            ctx.block->append(new MvOp(rd, MCOperand(Reg::a0)));
        }
    }
}

// ============================================================================
// 高级控制流 (TODO)
// ============================================================================

void InstSelPass::selectIf(IfInst* inst, InstSelContext& ctx) {
    std::cerr << "IfInst lowering not implemented yet" << std::endl;
}

void InstSelPass::selectWhile(WhileInst* inst, InstSelContext& ctx) {
    std::cerr << "WhileInst lowering not implemented yet" << std::endl;
}

void InstSelPass::selectBreak(BreakInst* inst, InstSelContext& ctx) {
    std::cerr << "BreakInst lowering not implemented yet" << std::endl;
}

void InstSelPass::selectContinue(ContinueInst* inst, InstSelContext& ctx) {
    std::cerr << "ContinueInst lowering not implemented yet" << std::endl;
}

// ============================================================================
// Phi 节点
// ============================================================================

void InstSelPass::selectPhi(PhiInst* inst, InstSelContext& ctx) {
    // Phi 节点需要在寄存器分配后消除
    ctx.getVReg(inst, InstSelContext::isFloatType(inst->getType()));
}

} // namespace rv
} // namespace sysy
