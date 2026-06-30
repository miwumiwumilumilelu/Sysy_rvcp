#include "../../../include/Optimize/Scalar/SSAInline.h"
#include "../../../include/Optimize/Scalar/IRClone.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/LoopInfo.h"
#include "../../../include/Optimize/Analysis/SCEV.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include <algorithm>
#include <assert.h>
#include <unordered_map>
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

static bool hasSmallTrip(Loop* L, SCEV& scev, int64_t maxTrip) {
    for (auto* cur = L; cur; cur = cur->up) {
        int64_t trips = -1;
        ExitBranchInfo info;
        bool ok = (cur->latch &&
                   analyzeExitBranch(cur, cur->latch, scev, info) &&
                   getConstantTripCountFromInfo(info, cur, trips) ||
                  (cur->head &&
                   analyzeExitBranch(cur, cur->head, scev, info) &&
                   getConstantTripCountFromInfo(info, cur, trips)
                  )
        );
        if (!ok || trips < 0 || trips > maxTrip)
            return false;
    }
    return true;
}

bool SSAInline::isInlineable(CallInst* call, Loop* callSiteLoop, SCEV* scev, int callSiteCount) const {
    Function* f = call ? call->getFunction() : nullptr;
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;
    if (isRecursive(f)) return false;
    bool calleeHasLoop = hasLoop(f);
    bool calleeWritesGlobal = writesGlobal(f);
    if (calleeHasLoop && calleeWritesGlobal) return false;
    if (!calleeHasLoop && calleeWritesGlobal && callSiteCount > 1) return false;

    // If the callee contains a loop and the call site is also within a loop, 
    // inlining is permitted only if the trip count of the loop containing the call site is small 
    // and can be proven Trip <= 8.
    if (calleeHasLoop && callSiteLoop && (!scev || !hasSmallTrip(callSiteLoop, *scev, 8))) return false;
    
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
    std::unordered_map<Function*, int> callSiteCounts;
    for (auto func : M->getFunctions())
        for (auto bb : func->getBody()->getBlocks())
            for (auto inst : bb->getInstructions())
                if (auto* call = dyn_cast<CallInst>(inst))
                    ++callSiteCounts[call->getFunction()];

    for (auto func : M->getFunctions()) {
        std::vector<CallInst*> toInline;
        Dominators dt(func);
        dt.run();
        LoopInfo li(func, dt);
        SCEV scev(func, li);
        for (auto bb : func->getBody()->getBlocks())
            for (auto inst : bb->getInstructions())
                if (auto call = dyn_cast<CallInst>(inst))
                    if (isInlineable(call, li.loopOf(bb), &scev, callSiteCounts[call->getFunction()]))
                        toInline.push_back(call);

        for (auto call : toInline) {
            doInline(call);
            anyChanged = true;
        }
    }

    for (auto func : M->getFunctions())
        AllocaHoist(func);

    return anyChanged;
}
