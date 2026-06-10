#include "../../../include/Optimize/High/HighDCE.h"
#include "../../../include/IR/IRRewriter.h"
#include <climits>
#include <cstdint>
#include <cstring>

namespace sysy {

static int evalICmp(ICmpInst::CmpOp op, int a, int b) {
    switch (op) {
        case ICmpInst::EQ: return a == b ? 1 : 0;
        case ICmpInst::NE: return a != b ? 1 : 0;
        case ICmpInst::SGT: return a > b  ? 1 : 0;
        case ICmpInst::SGE: return a >= b ? 1 : 0;
        case ICmpInst::SLT: return a < b  ? 1 : 0;
        case ICmpInst::SLE: return a <= b ? 1 : 0;
    }
    return 0;
}

static int asInt32(uint32_t value) {
    int32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return static_cast<int>(result);
}

static bool tryFoldInt(Instruction::OpID op, int a, int b, int& out) {
    uint32_t ua = static_cast<uint32_t>(a);
    uint32_t ub = static_cast<uint32_t>(b);
    switch (op) {
        case Instruction::Add: out = asInt32(ua + ub); return true;
        case Instruction::Sub: out = asInt32(ua - ub); return true;
        case Instruction::Mul: out = asInt32(ua * ub); return true;
        case Instruction::Div:
            if (!b || (a == INT_MIN && b == -1)) return false;
            out = a / b;
            return true;
        case Instruction::Mod:
            if (!b || (a == INT_MIN && b == -1)) return false;
            out = a % b;
            return true;
        case Instruction::Shl:
            if (b < 0 || b >= 32) return false;
            out = asInt32(ua << b); return true;
        case Instruction::Ashr:
            if (b < 0 || b >= 32) return false;
            if (b == 0) {
                out = a;
            } else if (ua & 0x80000000u) {
                out = asInt32((ua >> b) | (0xFFFFFFFFu << (32 - b)));
            } else {
                out = asInt32(ua >> b);
            }
            return true;
        case Instruction::And:  out = asInt32(ua & ub); return true;
        default: return false;
    }
}

static bool foldInst(Instruction* inst) {
    if (auto* icmp = dyn_cast<ICmpInst>(inst)) {
        auto* lc = dyn_cast<ConstantInt>(icmp->getOperand(0));
        auto* rc = dyn_cast<ConstantInt>(icmp->getOperand(1));
        if (lc && rc) {
            int result = evalICmp(icmp->getPredicate(), lc->getValue(), rc->getValue());
            icmp->replaceAllUsesWith(new ConstantInt(result));
            IRRewriter::eraseOp(icmp);
            return true;
        }
        return false;
    }

    if (auto* bin = dyn_cast<BinaryInst>(inst)) {
        auto* lc = dyn_cast<ConstantInt>(bin->getOperand(0));
        auto* rc = dyn_cast<ConstantInt>(bin->getOperand(1));
        if (lc && rc) {
            int result = 0;
            if (tryFoldInt(bin->getOpID(), lc->getValue(), rc->getValue(), result)) {
                bin->replaceAllUsesWith(new ConstantInt(result));
                IRRewriter::eraseOp(bin);
                return true;
            }
        }
        return false;
    }

    if (auto* ii = dyn_cast<IfInst>(inst)) {
        auto* cc = dyn_cast<ConstantInt>(ii->getOperand(0));
        if (cc)
            return IRRewriter::inlineSelectedBranch(ii, cc->getValue() != 0);
        return false;
    }

    // Dead pure instruction with no remaining uses
    if (!inst->getType()->isVoid() && inst->getUsers().empty() && inst->isPureCloneable()) {
        IRRewriter::eraseOp(inst);
        return true;
    }

    return false;
}

bool HighDCE::processFunc(Function* f) {
    return IRRewriter::rewriteRegion(f->getBody(), foldInst);
}

bool HighDCE::run() {
    bool changed = false;
    for (auto* func : M->getFunctions())
        changed |= processFunc(func);
    return changed;
}

}
