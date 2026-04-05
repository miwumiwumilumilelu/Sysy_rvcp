#include "Optimize/Scalar/SSAInline.h"
#include "IR/IRBuilder.h"
#include <algorithm>
#include <assert.h>
#include <vector>

using namespace sysy;

// Check if the function is calling itself.
bool SSAInline::isRecursive(Function* f) {
    const std::string& name = f->getName();
    for (auto bb : f->getBody()->getBlocks())
        for (auto inst : bb->getInstructions())
            if (auto call = dyn_cast<CallInst>(inst))
                if (call->getFunction()->getName() == name)
                    return true;
    return false;
}

int SSAInline::countInsts(Function* f) {
    int n = 0;
    for (auto bb : f->getBody()->getBlocks())
        n += (int)bb->getInstructions().size();
    return n;
}

bool SSAInline::isInlineable(Function* f) const {
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;
    if (isRecursive(f)) return false;
    int insts = countInsts(f);
    if (insts > threshold) return false;
    return true;
}

Value* SSAInline::remap(Value* v,
                        const std::map<Value*, Value*>& vmap,
                        const std::map<BasicBlock*, BasicBlock*>& bbMap) {
    if (!v) return nullptr;
    // bb
    if (auto bb = dyn_cast<BasicBlock>(v)) {
        auto it = bbMap.find(bb);
        return (it != bbMap.end()) ? it->second : v;
    }
    // value
    auto it = vmap.find(v);
    if (it != vmap.end()) return it->second;
    return v;
}

Instruction* SSAInline::cloneNonPhiInst(Instruction* inst, BasicBlock* target,
                                        const std::map<Value*, Value*>& vmap,
                                        const std::map<BasicBlock*, BasicBlock*>& bbMap) {
    Instruction* clone = nullptr;

    switch (inst->getOpID()) {
        case Instruction::Add:  case Instruction::Sub:
        case Instruction::Mul:  case Instruction::Div:  case Instruction::Mod:
        case Instruction::FAdd: case Instruction::FSub:
        case Instruction::FMul: case Instruction::FDiv: {
            auto* c = new BinaryInst(inst->getOpID(), nullptr, nullptr, nullptr);
            c->setOperand(0, remap(inst->getOperand(0), vmap, bbMap));
            c->setOperand(1, remap(inst->getOperand(1), vmap, bbMap));
            clone = c;
            break;
        }
        case Instruction::ICmp: {
            auto* orig = cast<ICmpInst>(inst);
            auto* c = new ICmpInst(orig->getPredicate(), nullptr, nullptr, nullptr);
            c->setOperand(0, remap(orig->getOperand(0), vmap, bbMap));
            c->setOperand(1, remap(orig->getOperand(1), vmap, bbMap));
            clone = c;
            break;
        }
        case Instruction::FCmp: {
            auto* orig = cast<FCmpInst>(inst);
            auto* c = new FCmpInst(orig->getPredicate(), nullptr, nullptr, nullptr);
            c->setOperand(0, remap(orig->getOperand(0), vmap, bbMap));
            c->setOperand(1, remap(orig->getOperand(1), vmap, bbMap));
            clone = c;
            break;
        }
        case Instruction::SIToFP:
        case Instruction::FPToSI: {
            auto* orig = cast<CastInst>(inst);
            auto* c = new CastInst(orig->getOpID(), nullptr, orig->getType(), nullptr);
            c->setOperand(0, remap(orig->getOperand(0), vmap, bbMap));
            clone = c;
            break;
        }
        case Instruction::Alloca: {
            auto* orig = cast<AllocaInst>(inst);
            clone = new AllocaInst(orig->getAllocatedType(), nullptr);
            break;
        }
        case Instruction::Load: {
            Value* origPtr = inst->getOperand(0);
            auto* c = new LoadInst(origPtr, nullptr);
            c->setOperand(0, remap(origPtr, vmap, bbMap));
            clone = c;
            break;
        }
        case Instruction::Store: {
            auto* c = new StoreInst(nullptr, nullptr, nullptr);
            c->setOperand(0, remap(inst->getOperand(0), vmap, bbMap));
            c->setOperand(1, remap(inst->getOperand(1), vmap, bbMap));
            clone = c;
            break;
        }
        case Instruction::GetElementPtr: {
            Value* origBase = inst->getOperand(0);
            auto* c = new GetElementPtrInst(origBase, origBase, nullptr);
            c->setOperand(0, remap(origBase, vmap, bbMap));
            c->setOperand(1, remap(inst->getOperand(1), vmap, bbMap));
            clone = c;
            break;
        }
        case Instruction::Br: {
            if (inst->getNumOperands() == 1) {
                auto* c = new BranchInst(static_cast<BasicBlock*>(nullptr), nullptr);
                c->setOperand(0, remap(inst->getOperand(0), vmap, bbMap));
                clone = c;
            } else {
                auto* c = new BranchInst(nullptr,
                                        static_cast<BasicBlock*>(nullptr),
                                        static_cast<BasicBlock*>(nullptr),
                                        nullptr);
                c->setOperand(0, remap(inst->getOperand(0), vmap, bbMap));
                c->setOperand(1, remap(inst->getOperand(1), vmap, bbMap));
                c->setOperand(2, remap(inst->getOperand(2), vmap, bbMap));
                clone = c;
            }
            break;
        }
        case Instruction::Ret:
        case Instruction::Phi:
            return nullptr;
        case Instruction::Call: {
            auto* orig = cast<CallInst>(inst);
            std::vector<Value*> args;
            for (int i = 1; i < orig->getNumOperands(); i++)
                args.push_back(remap(orig->getOperand(i), vmap, bbMap));
            clone = new CallInst(orig->getFunction(), args, nullptr);
            break;
        }
        default:
            assert(false && "SSAInline: unsupported instruction type");
            return nullptr;
    }

    clone->setName(inst->getName());
    clone->setParent(target);
    target->getInstructions().push_back(clone);
    return clone;
}

// callBB -> nextBBs
//
// after inline:
//
// callBB -> callee_bb0 -> callee_bb1 -> ... -> callee_bbN -> endBB -> nextBBs
void SSAInline::doInline(CallInst* call) {
    Function* callee = call->getFunction();
    BasicBlock* callBB = call->getParent();
    Region* callRegion = callBB->getParent();

    std::vector<BasicBlock*> origSuccs;
    {
        auto& insts = callBB->getInstructions();
        if (!insts.empty()) {
            if (auto br = dyn_cast<BranchInst>(insts.back())) {
                for (int i = (br->getNumOperands() == 1 ? 0 : 1); i < br->getNumOperands(); ++i) {
                    if (auto* succ = dyn_cast<BasicBlock>(br->getOperand(i)))
                        origSuccs.push_back(succ);
                }
            }
        }
    }

    BasicBlock* endBB = new BasicBlock(callee->getName() + "_end", nullptr);

    {
        auto& callInsts = callBB->getInstructions();
        auto callIt = std::find(callInsts.begin(), callInsts.end(), call);
        assert(callIt != callInsts.end());
        auto afterCall = std::next(callIt);
        endBB->getInstructions().splice(
            endBB->getInstructions().begin(),
            callInsts, afterCall, callInsts.end());
        for (auto inst : endBB->getInstructions())
            inst->setParent(endBB);
    }

    std::map<BasicBlock*, BasicBlock*> bbMap;
    // Record the old blocks of the callee.
    std::vector<BasicBlock*> calleeBlocks;

    for (auto bb : callee->getBody()->getBlocks()) {
        auto* cloned = new BasicBlock(callee->getName() + "_" + bb->getName(), nullptr);
        bbMap[bb] = cloned;
        calleeBlocks.push_back(bb);
    }

    {
        auto& blocks  = callRegion->getBlocks();
        // Find the next bb after the callBB.
        auto  insPos  = std::next(std::find(blocks.begin(), blocks.end(), callBB));
        for (auto origBB : calleeBlocks) {
            auto* c = bbMap[origBB];
            c->setParent(callRegion);
            blocks.insert(insPos, c);
        }
        endBB->setParent(callRegion);
        blocks.insert(insPos, endBB);
    }

    std::map<Value*, Value*> vmap;
    // Map callee arguments to caller operands.
    const auto& fargs = callee->getArgs();
    for (int i = 0; i < (int)fargs.size(); ++i)
        vmap[fargs[i]] = call->getOperand(i + 1);

    // Firstly clone phis, but no set operands.
    struct PhiPair { PhiInst* orig; PhiInst* clone; };
    std::vector<PhiPair> pendingPhis;

    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];

        for (auto inst : origBB->getInstructions()) {
            if (auto phi = dyn_cast<PhiInst>(inst)) {
                auto* cphi = new PhiInst(phi->getType(), nullptr);
                cphi->setName(phi->getName());
                cphi->setParent(clonedBB);
                clonedBB->getInstructions().push_back(cphi);
                vmap[phi] = cphi;
                pendingPhis.push_back({phi, cphi});
            }
        }
    }

    // Secondly clone non-Phi/Br/Ret insts.
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];

        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret ||
                inst->getOpID() == Instruction::Phi ||
                inst->getOpID() == Instruction::Br)
                continue;

            auto* cloned = cloneNonPhiInst(inst, clonedBB, vmap, bbMap);
            vmap[inst] = cloned;
        }
    }

    // Set operands for pending Phis after all non-Phi/Br/Ret clones are ready.
    for (auto [origPhi, cphi] : pendingPhis) {
        for (int i = 0; i < origPhi->getNumOperands(); i += 2) {
            Value* val = remap(origPhi->getOperand(i), vmap, bbMap);
            auto* bb = cast<BasicBlock>(remap(origPhi->getOperand(i+1), vmap, bbMap));
            cphi->addIncoming(val, bb);
        }
    }

    std::vector<std::pair<Value*, BasicBlock*>> returns;
    
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];

        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Phi) continue;

            if (inst->getOpID() == Instruction::Ret) {
                Value* retVal = (inst->getNumOperands() > 0)
                                ? remap(inst->getOperand(0), vmap, bbMap)
                                : nullptr;
                returns.push_back({retVal, clonedBB});
                // ret -> br endBB
                auto* br = new BranchInst(endBB, nullptr);
                br->setParent(clonedBB);
                clonedBB->getInstructions().push_back(br);
                continue;
            }

            if (inst->getOpID() == Instruction::Br) {
                cloneNonPhiInst(inst, clonedBB, vmap, bbMap);
                continue;
            }
        }
    }

    if (!returns.empty()) {
        if (returns.size() == 1) {
            call->replaceAllUsesWith(returns[0].first);
        } else {
            // Insert Phi in endBB to merge multiple return values.
            auto* phi = new PhiInst(callee->getType(), nullptr);
            for (auto [val, bb] : returns)
                phi->addIncoming(val, bb);
            phi->setParent(endBB);
            endBB->getInstructions().push_front(phi);
            call->replaceAllUsesWith(phi);
        }
    }

    // Delete CallInst in callBB, then new jump to the first cloned block of the callee.
    {
        auto& callInsts = callBB->getInstructions();
        auto callIt = std::find(callInsts.begin(), callInsts.end(), call);
        assert(callIt != callInsts.end());
        callInsts.erase(callIt);
        call->setParent(nullptr);
        delete call;

        auto* firstClone = bbMap[callee->getBody()->getEntryBlock()];
        auto* br = new BranchInst(firstClone, nullptr);
        br->setParent(callBB);
        callBB->getInstructions().push_back(br);
    }

    for (auto* succ : origSuccs) {
        for (auto inst : succ->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            for (int i = 1; i < phi->getNumOperands(); i += 2) {
                if (phi->getOperand(i) == callBB) {
                    phi->setOperand(i, endBB);
                }
            }
        }
    }
}

void SSAInline::AllocaHoist(Function* func) {
    if (!func || func->getBody()->getBlocks().empty()) return;

    BasicBlock* entry = func->getEntryBlock();
    if (!entry) return;

    auto& entryInsts = entry->getInstructions();
    auto insertPos = entryInsts.begin();
    while (insertPos != entryInsts.end() && isa<AllocaInst>(*insertPos))
        ++insertPos;

    std::vector<AllocaInst*> allocas;
    for (auto bb : func->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto alloca = dyn_cast<AllocaInst>(inst)) {
                if (alloca->getParent() != entry || std::find(entryInsts.begin(), insertPos, alloca) == entryInsts.end())
                    allocas.push_back(alloca);
            }
        }
    }

    for (auto* alloca : allocas) {
        BasicBlock* parent = alloca->getParent();
        if (!parent) continue;
        auto& insts = parent->getInstructions();
        auto it = std::find(insts.begin(), insts.end(), alloca);
        if (it == insts.end()) continue;
        entryInsts.splice(insertPos, insts, it);
        alloca->setParent(entry);
    }
}

bool SSAInline::run() {
    bool anyChanged = false;
    bool changed;

    // do-while support cascading inline.
    do {
        changed = false;
        for (auto func : M->getFunctions()) {
            std::vector<CallInst*> toInline;

            for (auto bb : func->getBody()->getBlocks()) {
                for (auto inst : bb->getInstructions()) {
                    if (auto call = dyn_cast<CallInst>(inst)) {
                        if (isInlineable(call->getFunction()))
                            toInline.push_back(call);
                    }
                }
            }

            for (auto call : toInline) {
                doInline(call);
                changed = true;
            }
        }

        anyChanged |= changed;
    } while (changed);

    for (auto func : M->getFunctions())
        AllocaHoist(func);

    return anyChanged;
}
