#include "Optimize/Scalar/SSAInline.h"
#include "Optimize/Scalar/IRClone.h"
#include <algorithm>
#include <assert.h>
#include <string>
#include <vector>

using namespace sysy;

static int SSAInlineID = 0;

static std::string uniqueName(const std::string& seed) {
    if (!seed.empty())
        return seed + ".inl" + std::to_string(SSAInlineID++);
    return "%inl" + std::to_string(SSAInlineID++);
}

static void renameClone(Instruction* c) {
    if (c && !c->getType()->isVoid())
        c->setName(uniqueName(c->getName()));
}

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
    if (countInsts(f) > threshold) return false;
    return true;
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
    if (!callBB->getInstructions().empty()) {
        if (auto br = dyn_cast<BranchInst>(callBB->getInstructions().back())) {
            int start = (br->getNumOperands() == 1 ? 0 : 1);
            for (int i = start; i < br->getNumOperands(); ++i)
                if (auto* succ = dyn_cast<BasicBlock>(br->getOperand(i)))
                    origSuccs.push_back(succ);
        }
    }

    BasicBlock* endBB = new BasicBlock(uniqueName(callee->getName() + "_end"), nullptr);

    // Move everything after the call into endBB.
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

    // Pre-create cloned blocks for the callee and splice them after callBB.
    BlockMap bbMap;
    std::vector<BasicBlock*> calleeBlocks;
    for (auto bb : callee->getBody()->getBlocks()) {
        auto* cloned = new BasicBlock(uniqueName(callee->getName() + "_" + bb->getName()), nullptr);
        bbMap[bb] = cloned;
        calleeBlocks.push_back(bb);
    }
    {
        auto& blocks = callRegion->getBlocks();
        auto insPos = std::next(std::find(blocks.begin(), blocks.end(), callBB));
        for (auto origBB : calleeBlocks) {
            auto* c = bbMap[origBB];
            c->setParent(callRegion);
            blocks.insert(insPos, c);
        }
        endBB->setParent(callRegion);
        blocks.insert(insPos, endBB);
    }

    // Map callee arguments to caller operands.
    ValueMap vmap;
    const auto& fargs = callee->getArgs();
    for (int i = 0; i < (int)fargs.size(); ++i)
        vmap[fargs[i]] = call->getOperand(i + 1);

    // Two-pass clone: skeletons first so forward refs resolve when filling.
    // Branches and Returns are deferred — they drive the inlined control flow.
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret ||
                inst->getOpID() == Instruction::Br) continue;
            auto* c = cloneSkeleton(inst, clonedBB);
            renameClone(c);
            vmap[inst] = c;
        }
    }
    for (auto origBB : calleeBlocks) {
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret ||
                inst->getOpID() == Instruction::Br) continue;
            fillOperands(cast<Instruction>(vmap[inst]), inst, vmap, bbMap);
        }
    }

    // Route returns through endBB. Clone branches verbatim.
    std::vector<std::pair<Value*, BasicBlock*>> returns;
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret) {
                Value* rv = (inst->getNumOperands() > 0)
                            ? remapValue(inst->getOperand(0), vmap, bbMap)
                            : nullptr;
                returns.push_back({rv, clonedBB});
                new BranchInst(endBB, clonedBB);
            } else if (inst->getOpID() == Instruction::Br) {
                cloneInst(inst, clonedBB, vmap, bbMap);
            }
        }
    }

    // Hook return values.
    if (!returns.empty()) {
        if (returns.size() == 1) {
            call->replaceAllUsesWith(returns[0].first);
        } else {
            auto* phi = new PhiInst(callee->getType(), nullptr);
            for (auto [val, bb] : returns)
                phi->addIncoming(val, bb);
            phi->setParent(endBB);
            endBB->getInstructions().push_front(phi);
            call->replaceAllUsesWith(phi);
        }
    }

    // Remove the original call and branch from callBB into the first clone.
    {
        auto& callInsts = callBB->getInstructions();
        auto callIt = std::find(callInsts.begin(), callInsts.end(), call);
        assert(callIt != callInsts.end());
        callInsts.erase(callIt);
        call->setParent(nullptr);
        delete call;

        auto* firstClone = bbMap[callee->getBody()->getEntryBlock()];
        new BranchInst(firstClone, callBB);
    }

    // Retarget phi incomings in successors: callBB is replaced by endBB on the
    // fallthrough path.
    for (auto* succ : origSuccs) {
        for (auto inst : succ->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            for (int i = 1; i < phi->getNumOperands(); i += 2)
                if (phi->getOperand(i) == callBB)
                    phi->setOperand(i, endBB);
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
                if (alloca->getParent() != entry ||
                    std::find(entryInsts.begin(), insertPos, alloca) == entryInsts.end())
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
    do {
        changed = false;
        for (auto func : M->getFunctions()) {
            std::vector<CallInst*> toInline;
            for (auto bb : func->getBody()->getBlocks())
                for (auto inst : bb->getInstructions())
                    if (auto call = dyn_cast<CallInst>(inst))
                        if (isInlineable(call->getFunction()))
                            toInline.push_back(call);

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
