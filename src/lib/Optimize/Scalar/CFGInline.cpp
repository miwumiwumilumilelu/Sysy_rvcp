#include "Optimize/Scalar/CFGInline.h"
#include "Optimize/Scalar/IRClone.h"
#include "Optimize/Scalar/SSAInline.h"
#include <algorithm>
#include <assert.h>
#include <vector>

using namespace sysy;

bool CFGInline::hasLoop(Function* f) {
    if (!f || f->getBody()->getBlocks().empty()) return false;

    Dominators dt(f);
    dt.run();

    for (auto bb : f->getBody()->getBlocks()) {
        for (auto succ : dt.getSuccessors(bb)) {
            if (dt.dominates(succ, bb)) return true;
        }
    }
    return false;
}

bool CFGInline::hasPhi(Function* f) {
    for (auto bb : f->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (isa<PhiInst>(inst)) return true;
        }
    }
    return false;
}

bool CFGInline::hasAlloca(Function* f) {
    for (auto bb : f->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (isa<AllocaInst>(inst)) return true;
        }
    }
    return false;
}

bool CFGInline::hasNestedCall(Function* f) {
    for (auto bb : f->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            auto* call = dyn_cast<CallInst>(inst);
            if (!call) continue;
            if (call->getFunction() != f) return true;
        }
    }
    return false;
}

bool CFGInline::isInlineable(Function* f) const {
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;
    if (SSAInline::isRecursive(f)) return false;
    if (hasLoop(f)) return false;
    if (hasPhi(f)) return false;
    if (hasAlloca(f)) return false;
    if (hasNestedCall(f)) return false;
    if ((int)f->getBody()->getBlocks().size() > 4) return false;
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

    BlockMap bbMap;
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

    ValueMap vmap;
    const auto& fargs = callee->getArgs();
    for (int i = 0; i < (int)fargs.size(); ++i)
        vmap[fargs[i]] = call->getOperand(i + 1);

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

    // Two-pass clone: skeletons then operands, so forward refs resolve.
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret) continue;
            auto* c = cloneSkeleton(inst, clonedBB);
            vmap[inst] = c;
        }
    }
    for (auto origBB : calleeBlocks) {
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret) continue;
            fillOperands(cast<Instruction>(vmap[inst]), inst, vmap, bbMap);
        }
    }

    // Route returns to endBB, storing the value into retAddr.
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() != Instruction::Ret) continue;
            if (retAddr && inst->getNumOperands() > 0)
                new StoreInst(remapValue(inst->getOperand(0), vmap, bbMap),
                              retAddr, clonedBB);
            new BranchInst(endBB, clonedBB);
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
        call->eraseInst();

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
