#include "Optimize/Scalar/IRClone.h"
#include "IR/Module.h"
#include <cassert>
#include <vector>

using namespace sysy;

Value* sysy::remapValue(Value* v, const ValueMap& vmap, const BlockMap& bbMap) {
    if (!v) return nullptr;
    if (auto* bb = dyn_cast<BasicBlock>(v)) {
        auto it = bbMap.find(bb);
        return (it != bbMap.end()) ? it->second : v;
    }
    auto it = vmap.find(v);
    return (it != vmap.end()) ? it->second : v;
}

Instruction* sysy::cloneSkeleton(Instruction* src, BasicBlock* target) {
    Instruction* c = nullptr;
    switch (src->getOpID()) {
    case Instruction::Add:  case Instruction::Sub:
    case Instruction::Mul:  case Instruction::Div:  case Instruction::Mod:
    case Instruction::Shl:  case Instruction::Ashr: case Instruction::And:
    case Instruction::Or:   case Instruction::Xor:
    case Instruction::FAdd: case Instruction::FSub:
    case Instruction::FMul: case Instruction::FDiv:
        c = new BinaryInst(src->getOpID(), nullptr, nullptr, nullptr);
        break;
    case Instruction::ICmp:
        c = new ICmpInst(cast<ICmpInst>(src)->getPredicate(), nullptr, nullptr, nullptr);
        break;
    case Instruction::FCmp:
        c = new FCmpInst(cast<FCmpInst>(src)->getPredicate(), nullptr, nullptr, nullptr);
        break;
    case Instruction::SIToFP: case Instruction::FPToSI:
        c = new CastInst(src->getOpID(), nullptr, src->getType(), nullptr);
        break;
    case Instruction::Alloca:
        c = new AllocaInst(cast<AllocaInst>(src)->getAllocatedType(), nullptr);
        break;
    case Instruction::Load:
        c = new LoadInst(src->getOperand(0), nullptr);
        break;
    case Instruction::Store:
        c = new StoreInst(nullptr, nullptr, nullptr);
        break;
    case Instruction::GetElementPtr:
        c = new GetElementPtrInst(src->getOperand(0), nullptr, nullptr);
        // preserve GEP indexedtype when cloning IR
        cast<GetElementPtrInst>(c)->setIndexedType(cast<GetElementPtrInst>(src)->getIndexedType());
        break;
    case Instruction::Br:
        if (src->getNumOperands() == 1)
            c = new BranchInst(static_cast<BasicBlock*>(nullptr), nullptr);
        else
            c = new BranchInst(nullptr,
                               static_cast<BasicBlock*>(nullptr),
                               static_cast<BasicBlock*>(nullptr),
                               nullptr);
        break;
    case Instruction::Ret: {
        // ReturnInst only allocates operand storage when the initial value
        // is non-null; seed the original operand so fillOperands() can later
        // overwrite via setOperand().
        Value* v = (src->getNumOperands() > 0) ? src->getOperand(0) : nullptr;
        c = new ReturnInst(v, nullptr);
        break;
    }
    case Instruction::Call: {
        auto* call = cast<CallInst>(src);
        std::vector<Value*> args(call->getNumOperands() - 1, nullptr);
        c = new CallInst(call->getFunction(), args, nullptr);
        break;
    }
    case Instruction::Phi:
        c = new PhiInst(src->getType(), nullptr);
        break;
    default:
        return nullptr;
    }
    c->setName(src->getName());
    c->setParent(target);
    target->getInstructions().push_back(c);
    return c;
}


void sysy::fillOperands(Instruction* clone, Instruction* src,
                        const ValueMap& vmap, const BlockMap& bbMap) {
    auto rm = [&](Value* v) { return remapValue(v, vmap, bbMap); };

    switch (src->getOpID()) {
    case Instruction::Add:  case Instruction::Sub:
    case Instruction::Mul:  case Instruction::Div:  case Instruction::Mod:
    case Instruction::Shl:  case Instruction::Ashr: case Instruction::And:
    case Instruction::Or:   case Instruction::Xor:
    case Instruction::FAdd: case Instruction::FSub:
    case Instruction::FMul: case Instruction::FDiv:
    case Instruction::ICmp: case Instruction::FCmp:
    case Instruction::Store: case Instruction::GetElementPtr:
        clone->setOperand(0, rm(src->getOperand(0)));
        clone->setOperand(1, rm(src->getOperand(1)));
        return;

    case Instruction::SIToFP: case Instruction::FPToSI:
    case Instruction::Load:
        clone->setOperand(0, rm(src->getOperand(0)));
        return;

    case Instruction::Br:
        clone->setOperand(0, rm(src->getOperand(0)));
        if (src->getNumOperands() == 3) {
            clone->setOperand(1, rm(src->getOperand(1)));
            clone->setOperand(2, rm(src->getOperand(2)));
        }
        return;

    case Instruction::Ret:
        if (src->getNumOperands() > 0)
            clone->setOperand(0, rm(src->getOperand(0)));
        return;

    case Instruction::Call:
        for (int i = 1; i < src->getNumOperands(); ++i)
            clone->setOperand(i, rm(src->getOperand(i)));
        return;

    case Instruction::Phi: {
        auto* sp = cast<PhiInst>(src);
        auto* cp = cast<PhiInst>(clone);
        for (int i = 0; i < sp->getNumOperands(); i += 2) {
            Value* v = rm(sp->getOperand(i));
            auto* bb = cast<BasicBlock>(rm(sp->getOperand(i + 1)));
            cp->addIncoming(v, bb);
        }
        return;
    }

    case Instruction::Alloca:
        return;

    default:
        assert(false && "IRClone: unsupported opcode in fillOperands");
        return;
    }
}

Instruction* sysy::cloneInst(Instruction* src, BasicBlock* target,
                             const ValueMap& vmap, const BlockMap& bbMap) {
    auto* c = cloneSkeleton(src, target);
    if (!c) return nullptr;
    fillOperands(c, src, vmap, bbMap);
    return c;
}
