#ifndef SYSY_RV_INSTSEL_H
#define SYSY_RV_INSTSEL_H

#include "IR/Instruction.h"
#include "IR/Module.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include "IR/Region.h"
#include "rv/MCFunction.h"
#include "rv/RvOp.h"
#include <unordered_map>
#include <vector>

namespace sysy {
namespace rv {

// 值映射：IR Value → MCOperand (VReg)
using ValueMap = std::unordered_map<Value*, MCOperand>;

// 指令选择上下文
class InstSelContext {
public:
    MCFunction* func = nullptr;      // 当前构建的函数
    MCBlock* block = nullptr;        // 当前基本块
    ValueMap valueMap;               // IR Value → VReg 映射

    // 获取或创建 VReg
    MCOperand getVReg(Value* v, bool isFloat = false);

    // 创建新的 VReg
    MCOperand newVReg(bool isFloat = false);

    // 类型判断辅助
    static bool isFloatType(Type* ty) { return ty && ty->isFloat(); }
    static bool isIntType(Type* ty) { return ty && ty->isInt(); }
};

// 指令选择 Pass：将 IR 转换为 RvOp
class InstSelPass {
public:
    // 执行指令选择，返回所有函数的 MCFunction
    std::vector<std::unique_ptr<MCFunction>> run(Module* module);

private:
    // 选择单个函数
    MCFunction* selectFunction(Function* irFunc);

    // 选择单个基本块
    void selectBasicBlock(BasicBlock* irBB, InstSelContext& ctx);

    // 选择单条指令
    void selectInstruction(Instruction* inst, InstSelContext& ctx);

    // ========================================================================
    // 指令特定的选择方法
    // ========================================================================

    // 算术运算
    void selectAdd(BinaryInst* inst, InstSelContext& ctx);
    void selectSub(BinaryInst* inst, InstSelContext& ctx);
    void selectMul(BinaryInst* inst, InstSelContext& ctx);
    void selectDiv(BinaryInst* inst, InstSelContext& ctx);
    void selectMod(BinaryInst* inst, InstSelContext& ctx);

    // 浮点运算
    void selectFAdd(BinaryInst* inst, InstSelContext& ctx);
    void selectFSub(BinaryInst* inst, InstSelContext& ctx);
    void selectFMul(BinaryInst* inst, InstSelContext& ctx);
    void selectFDiv(BinaryInst* inst, InstSelContext& ctx);

    // 内存访问
    void selectAlloca(AllocaInst* inst, InstSelContext& ctx);
    void selectLoad(LoadInst* inst, InstSelContext& ctx);
    void selectStore(StoreInst* inst, InstSelContext& ctx);
    void selectGetElementPtr(GetElementPtrInst* inst, InstSelContext& ctx);

    // 类型转换
    void selectSIToFP(CastInst* inst, InstSelContext& ctx);
    void selectFPToSI(CastInst* inst, InstSelContext& ctx);

    // 比较
    void selectICmp(ICmpInst* inst, InstSelContext& ctx);
    void selectFCmp(FCmpInst* inst, InstSelContext& ctx);

    // 控制流
    void selectBranch(BranchInst* inst, InstSelContext& ctx);
    void selectReturn(ReturnInst* inst, InstSelContext& ctx);
    void selectCall(CallInst* inst, InstSelContext& ctx);

    // 高级控制流
    void selectIf(IfInst* inst, InstSelContext& ctx);
    void selectWhile(WhileInst* inst, InstSelContext& ctx);
    void selectBreak(BreakInst* inst, InstSelContext& ctx);
    void selectContinue(ContinueInst* inst, InstSelContext& ctx);

    // Phi 节点
    void selectPhi(PhiInst* inst, InstSelContext& ctx);
};

} // namespace rv
} // namespace sysy

#endif
