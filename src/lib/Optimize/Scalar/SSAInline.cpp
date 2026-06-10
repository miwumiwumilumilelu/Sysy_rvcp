#include "../../../include/Optimize/Scalar/SSAInline.h"
#include "../../../include/Optimize/Scalar/IRClone.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include <algorithm>
#include <assert.h>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace sysy;

static int SSAInlineID = 0;

static std::string inlineName(const std::string& seed, int siteID) {
    if (!seed.empty())
        return seed + ".i" + std::to_string(siteID);
    return "%i" + std::to_string(siteID);
}

static void renameClone(Instruction* c, int siteID) {
    if (c && !c->getType()->isVoid())
        c->setName(inlineName(c->getName(), siteID));
}

bool SSAInline::isRecursive(Function* f) {
    for (auto bb : f->getBody()->getBlocks())
        for (auto inst : bb->getInstructions())
            if (auto call = dyn_cast<CallInst>(inst))
                if (call->getFunction() == f)
                    return true;
    return false;
}

int SSAInline::countInsts(Function* f) {
    int n = 0;
    for (auto bb : f->getBody()->getBlocks())
        n += (int)bb->getInstructions().size();
    return n;
}

static std::vector<BasicBlock*> successorsOf(BasicBlock* bb) {
    std::vector<BasicBlock*> succs;
    if (bb->getInstructions().empty()) return succs;
    auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
    if (!br) return succs;

    int start = (br->getNumOperands() == 1 ? 0 : 1);
    for (int i = start; i < br->getNumOperands(); ++i) {
        if (auto* target = dyn_cast<BasicBlock>(br->getOperand(i)))
            succs.push_back(target);
    }
    return succs;
}

static bool hasLoop(Function* f) {
    Dominators dom(f);
    dom.run();
    for (auto bb : f->getBody()->getBlocks()) {
        for (auto* succ : successorsOf(bb))
            if (dom.dominates(succ, bb))
                return true;
    }
    return false;
}

static bool isDerivedFromGlobal(Value* v) {
    if (isa<GlobalVariable>(v)) return true;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return isDerivedFromGlobal(gep->getOperand(0));
    return false;
}

static bool writesGlobal(Function* f) {
    for (auto bb : f->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                if (isDerivedFromGlobal(st->getOperand(1)))
                    return true;
            }
        }
    }
    return false;
}

static std::set<BasicBlock*> naturalLoopBlocks(Function* f) {
    std::map<BasicBlock*, std::vector<BasicBlock*>> preds;
    for (auto bb : f->getBody()->getBlocks()) {
        preds[bb];
        for (auto* succ : successorsOf(bb))
            preds[succ].push_back(bb);
    }

    Dominators dom(f);
    dom.run();

    std::set<BasicBlock*> loopBlocks;
    for (auto latch : f->getBody()->getBlocks()) {
        for (auto* header : successorsOf(latch)) {
            if (!dom.dominates(header, latch)) continue;

            std::vector<BasicBlock*> stack{latch};
            loopBlocks.insert(header);
            while (!stack.empty()) {
                BasicBlock* bb = stack.back();
                stack.pop_back();
                if (!loopBlocks.insert(bb).second) continue;
                for (auto* pred : preds[bb]) {
                    if (pred != header)
                        stack.push_back(pred);
                }
            }
        }
    }
    return loopBlocks;
}

bool SSAInline::isInlineable(CallInst* call, bool callSiteInLoop) const {
    Function* f = call ? call->getFunction() : nullptr;
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;
    if (isRecursive(f)) return false;
    if (callSiteInLoop && hasLoop(f) && writesGlobal(f)) return false;
    if (countInsts(f) > threshold) return false;
    return true;
}

// callBB -> nextBBs
//
// after inline:
//
// callBB -> callee_bb0 -> callee_bb1 -> ... -> callee_bbN -> endBB -> nextBBs
void SSAInline::doInline(CallInst* call) {
    int siteID = SSAInlineID++;
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

    BasicBlock* endBB = new BasicBlock(inlineName(callee->getName() + "_end", siteID), nullptr);

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
        auto* cloned = new BasicBlock(inlineName(callee->getName() + "_" + bb->getName(), siteID), nullptr);
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
            renameClone(c, siteID);
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
            if (!call->getName().empty())
                phi->setName(call->getName());
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
        call->eraseInst();

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
            auto loopBlocks = naturalLoopBlocks(func);
            for (auto bb : func->getBody()->getBlocks())
                for (auto inst : bb->getInstructions())
                    if (auto call = dyn_cast<CallInst>(inst))
                        if (isInlineable(call, loopBlocks.count(bb)))
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
