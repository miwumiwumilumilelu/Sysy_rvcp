#include "Optimize/Scalar/CFGInline.h"
#include "Optimize/Scalar/SSAInline.h"
#include <algorithm>
#include <assert.h>
#include <vector>

using namespace sysy;

bool CFGInline::isInlineable(Function* f) const {
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;
    if (SSAInline::isRecursive(f)) return false;
    return SSAInline::countInsts(f) <= threshold;
}

void CFGInline::doInline(CallInst* call) {
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
            endBB->getInstructions().begin(), callInsts, afterCall, callInsts.end());
        for (auto inst : endBB->getInstructions())
            inst->setParent(endBB);
    }

    std::map<BasicBlock*, BasicBlock*> bbMap;
    std::vector<BasicBlock*> calleeBlocks;
    for (auto bb : callee->getBody()->getBlocks()) {
        auto* cloned = new BasicBlock(callee->getName() + "_" + bb->getName(), nullptr);
        bbMap[bb] = cloned;
        calleeBlocks.push_back(bb);
    }

    {
        auto& blocks = callRegion->getBlocks();
        auto insPos = std::next(std::find(blocks.begin(), blocks.end(), callBB));
        for (auto origBB : calleeBlocks) {
            auto* cloned = bbMap[origBB];
            cloned->setParent(callRegion);
            blocks.insert(insPos, cloned);
        }
        endBB->setParent(callRegion);
        blocks.insert(insPos, endBB);
    }

    std::map<Value*, Value*> vmap;
    const auto& fargs = callee->getArgs();
    for (int i = 0; i < (int)fargs.size(); ++i)
        vmap[fargs[i]] = call->getOperand(i + 1);

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

    // ret -> alloca + store/load
    AllocaInst* retAddr = nullptr;
    if (!call->getType()->isVoid()) {
        retAddr = new AllocaInst(call->getType(), nullptr);
        retAddr->setName(callee->getName() + "_ret.addr");
        retAddr->setParent(callBB);
        auto& callInsts = callBB->getInstructions();
        auto callIt = std::find(callInsts.begin(), callInsts.end(), call);
        callInsts.insert(callIt, retAddr);
    }

    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Phi || inst->getOpID() == Instruction::Ret)
                continue;
            auto* cloned = SSAInline::cloneNonPhiInst(inst, clonedBB, vmap, bbMap);
            if (cloned)
                vmap[inst] = cloned;
        }
    }

    for (auto [origPhi, cphi] : pendingPhis) {
        for (int i = 0; i < origPhi->getNumOperands(); i += 2) {
            Value* val = SSAInline::remap(origPhi->getOperand(i), vmap, bbMap);
            auto* bb = cast<BasicBlock>(SSAInline::remap(origPhi->getOperand(i + 1), vmap, bbMap));
            cphi->addIncoming(val, bb);
        }
    }

    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() != Instruction::Ret) continue;

            if (retAddr && inst->getNumOperands() > 0) {
                auto* store = new StoreInst(SSAInline::remap(inst->getOperand(0), vmap, bbMap), retAddr, nullptr);
                store->setParent(clonedBB);
                clonedBB->getInstructions().push_back(store);
            }

            auto* br = new BranchInst(endBB, nullptr);
            br->setParent(clonedBB);
            clonedBB->getInstructions().push_back(br);
        }
    }

    if (retAddr) {
        auto* load = new LoadInst(retAddr, nullptr);
        load->setName(call->getName());
        load->setParent(endBB);
        endBB->getInstructions().push_front(load);
        call->replaceAllUsesWith(load);
    }

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
                if (phi->getOperand(i) == callBB)
                    phi->setOperand(i, endBB);
            }
        }
    }
}

void CFGInline::AllocaHoist(Function* func) {
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
            auto* alloca = dyn_cast<AllocaInst>(inst);
            if (!alloca) continue;
            if (alloca->getParent() == entry &&
                std::find(entryInsts.begin(), insertPos, alloca) != entryInsts.end())
                continue;
            allocas.push_back(alloca);
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

bool CFGInline::run() {
    bool anyChanged = false;
    bool changed;

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

            for (auto* call : toInline) {
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
