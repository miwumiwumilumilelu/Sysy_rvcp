#ifndef INSTSEL_H
#define INSTSEL_H

#include "IR/Instruction.h"
#include "IR/Module.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include "IR/Region.h"
#include "rv/MCFunction.h"
#include "rv/RvOp.h"
#include <map>
#include <unordered_map>
#include <vector>

namespace sysy {
namespace rv {

// IR Value → MCOperand (VReg)
using ValueMap = std::unordered_map<Value*, MCOperand>;

class InstSelContext {
public:
    MCFunction* func = nullptr;
    MCBlock* block = nullptr;
    ValueMap valueMap;

    // Eliminates duplicate slliw/mulw for GEPs sharing the same index in one block.
    std::map<std::pair<VReg, int>, MCOperand> scaledIndexCache;

    MCOperand getVReg(Value* v, bool isFloat = false);
    MCOperand newVReg(bool isFloat = false);

    static bool isFloatType(Type* ty) { return ty && ty->isFloat(); }
    static bool isIntType(Type* ty) { return ty && ty->isInt(); }
};

class InstSelPass {
public:
    std::vector<std::unique_ptr<MCFunction>> run(Module* module);

private:
    MCFunction* selectFunction(Function* irFunc);
    void selectBasicBlock(BasicBlock* irBB, InstSelContext& ctx);
    void selectInstruction(Instruction* inst, InstSelContext& ctx);

    void selectAdd(BinaryInst* inst, InstSelContext& ctx);
    void selectSub(BinaryInst* inst, InstSelContext& ctx);
    void selectMul(BinaryInst* inst, InstSelContext& ctx);
    void selectDiv(BinaryInst* inst, InstSelContext& ctx);
    void selectMod(BinaryInst* inst, InstSelContext& ctx);

    void selectFAdd(BinaryInst* inst, InstSelContext& ctx);
    void selectFSub(BinaryInst* inst, InstSelContext& ctx);
    void selectFMul(BinaryInst* inst, InstSelContext& ctx);
    void selectFDiv(BinaryInst* inst, InstSelContext& ctx);

    void selectAlloca(AllocaInst* inst, InstSelContext& ctx);
    void selectLoad(LoadInst* inst, InstSelContext& ctx);
    void selectStore(StoreInst* inst, InstSelContext& ctx);
    void selectGetElementPtr(GetElementPtrInst* inst, InstSelContext& ctx);

    void selectSIToFP(CastInst* inst, InstSelContext& ctx);
    void selectFPToSI(CastInst* inst, InstSelContext& ctx);

    void selectICmp(ICmpInst* inst, InstSelContext& ctx);
    void selectFCmp(FCmpInst* inst, InstSelContext& ctx);

    void selectBranch(BranchInst* inst, InstSelContext& ctx);
    void selectReturn(ReturnInst* inst, InstSelContext& ctx);
    void selectCall(CallInst* inst, InstSelContext& ctx);
    void selectIf(IfInst* inst, InstSelContext& ctx);
    void selectWhile(WhileInst* inst, InstSelContext& ctx);
    void selectBreak(BreakInst* inst, InstSelContext& ctx);
    void selectContinue(ContinueInst* inst, InstSelContext& ctx);

    void selectPhi(PhiInst* inst, InstSelContext& ctx);
};

} // namespace rv
} // namespace sysy

#endif
