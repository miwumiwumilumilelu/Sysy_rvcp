#include "Optimize/Scalar/GVNHoist.h"
#include "Optimize/Scalar/ExprKey.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

using namespace sysy;

// Safe to execute speculatively (no side effects).
// CallInst is excluded here; call hoisting uses isAnticipatable() instead.
static bool isSafe(Instruction* inst) {
    return isa<BinaryInst>(inst) || isa<ICmpInst>(inst) || isa<FCmpInst>(inst) ||
            isa<CastInst>(inst) || isa<GetElementPtrInst>(inst);
}

// Return the CFG successors of bb (from its BranchInst terminator).
static std::vector<BasicBlock*> getSuccessors(BasicBlock* bb) {
    std::vector<BasicBlock*> succs;
    auto& insts = bb->getInstructions();
    if (insts.empty()) return succs;
    auto* term = insts.back();
    auto* br = dyn_cast<BranchInst>(term);
    if (!br) return succs;   
    if (br->getNumOperands() == 1)
        succs.push_back(cast<BasicBlock>(br->getOperand(0)));
    else {
        succs.push_back(cast<BasicBlock>(br->getOperand(1)));  // ifTrue
        succs.push_back(cast<BasicBlock>(br->getOperand(2)));  // ifFalse
    }
    return succs;
}

// If the DFS completes without finding any uncovered exit -> anticipatable.
static bool isAnticipatable(const std::set<BasicBlock*>& callBlocks, BasicBlock* L) {
    std::set<BasicBlock*> visited;
    std::vector<BasicBlock*> worklist;
    for (auto* succ : getSuccessors(L)) {
        if (visited.insert(succ).second)
            worklist.push_back(succ);
    }
    while (!worklist.empty()) {
        auto* bb = worklist.back(); worklist.pop_back();
        if (callBlocks.count(bb)) continue;       // covered — prune this path
        auto succs = getSuccessors(bb);
        if (succs.empty()) return false;           // reached exit uncovered
        for (auto* s : succs)
            if (visited.insert(s).second)
                worklist.push_back(s);
    }
    return true;
}

// Clone inst (same operands) into tgt before its terminator.
static Instruction* cloneToBlock(Instruction* inst, BasicBlock* tgt) {
    auto op = inst->getOpID();
    auto o = [&](int i) { return inst->getOperand(i); };
    Instruction* cl = nullptr;
    if (isa<BinaryInst>(inst))
        cl = new BinaryInst(op, o(0), o(1), nullptr);
    else if (auto* ic = dyn_cast<ICmpInst>(inst))
        cl = new ICmpInst(ic->getPredicate(), o(0), o(1), nullptr);
    else if (auto* fc = dyn_cast<FCmpInst>(inst))
        cl = new FCmpInst(fc->getPredicate(), o(0), o(1), nullptr);
    else if (isa<CastInst>(inst))
        cl = new CastInst(op, o(0), inst->getType(), nullptr);
    else if (isa<GetElementPtrInst>(inst))
        cl = new GetElementPtrInst(o(0), o(1), nullptr);
    else if (auto* call = dyn_cast<CallInst>(inst)) {
        std::vector<Value*> args;
        for (int i = 1; i < (int)call->getNumOperands(); i++)
            args.push_back(call->getOperand(i));
        cl = new CallInst(call->getFunction(), args, nullptr);
    }
    if (!cl) return nullptr;
    cl->setParent(tgt);
    auto& ins = tgt->getInstructions();
    ins.insert(std::prev(ins.end()), cl);
    return cl;
}

bool GVNHoist::run() {
    bool any = false;
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool GVNHoist::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;

    Dominators dt(f); 
    dt.run();

    std::map<BasicBlock*, std::vector<BasicBlock*>> domCh;

    // LCA needs depth of each node.
    std::map<BasicBlock*, int> depth;
    for (auto bb : f->getBody()->getBlocks()) 
        domCh[bb] = {};
    for (auto bb : f->getBody()->getBlocks())
        if (auto* idom = dt.getIDom(bb)) 
            domCh[idom].push_back(bb);

    std::function<void(BasicBlock*, int)> computeDepth = [&](BasicBlock* bb, int d) {
        depth[bb] = d;
        for (auto ch : domCh[bb]) computeDepth(ch, d + 1);
    };
    computeDepth(f->getEntryBlock(), 0);

    // The one with the greater depth climbs up (idom) until the two meet.
    auto lca = [&](BasicBlock* a, BasicBlock* b) -> BasicBlock* {
        while (a != b) {
            if (!a || !b) return nullptr;
            if (depth[a] < depth[b]) std::swap(a, b);
            a = dt.getIDom(a);
        }
        return a;
    };

    // Check if all operands of inst are available in block L.
    auto operandsAvailAt = [&](Instruction* inst, BasicBlock* L) -> bool {
        for (int i = 0; i < (int)inst->getNumOperands(); i++) {
            Value* op = inst->getOperand(i);
            if (isa<Instruction>(op)) {
                auto* defBB = cast<Instruction>(op)->getParent();
                if (defBB != L && !dt.dominates(defBB, L)) return false;
            }
        }
        return true;
    };

    // Hoist a group of equivalent instructions to their LCA block.
    auto hoistGroup = [&](std::vector<Instruction*>& insts, 
                        std::vector<Instruction*>& toRemove) 
                        -> bool {

        if (insts.size() < 2) return false;

        std::set<BasicBlock*> blockSet;
        for (auto inst : insts) blockSet.insert(inst->getParent());

        // All in same block -> CSE handles. Skip.
        if (blockSet.size() < 2) return false;

        // Find LCA of all blocks.
        BasicBlock* L = *blockSet.begin();
        for (auto bb : blockSet) L = lca(L, bb);

        if (!L) return false;

        // LCA is one of the blocks -> GVN handles (dominator case). Skip.
        if (blockSet.count(L)) return false;

        if (!operandsAvailAt(insts[0], L)) return false;

        auto* hoisted = cloneToBlock(insts[0], L);
        if (!hoisted) return false;
        if (!insts[0]->getName().empty())
            hoisted->setName(insts[0]->getName());
        for (auto inst : insts) {
            inst->replaceAllUsesWith(hoisted);
            toRemove.push_back(inst);
        }
        return true;
    };

    // fix-point iteration
    std::unordered_map<Function*, bool> purityCache;
    bool anyTotal = false;
    bool changed = true;
    while (changed) {
        changed = false;

        // Rebuild groups each iteration since instruction positions changed.
        std::map<ExprKey, std::vector<Instruction*>> groups;
        std::map<CallKey, std::vector<Instruction*>> callGroups;

        for (auto bb : f->getBody()->getBlocks()) {
            for (auto inst : bb->getInstructions()) {
                if (auto* call = dyn_cast<CallInst>(inst)) {
                    if (call->getType()->isVoid()) continue;
                    if (!isPureFunc(call->getFunction(), purityCache)) continue;
                    CallKey k;
                    k.push_back((uint64_t)(uintptr_t)call->getFunction());
                    for (int i = 1; i < (int)call->getNumOperands(); i++)
                        k.push_back(vnKey(call->getOperand(i)));
                    callGroups[k].push_back(inst);
                    continue;
                }
                if (!isSafe(inst)) continue;
                ExprKey k = makeExprKey(inst);
                if (k == ExprKey{0, 0, 0}) continue;
                groups[k].push_back(inst);
            }
        }

        std::vector<Instruction*> toRemove;

        for (auto& [key, insts] : groups)
            if (hoistGroup(insts, toRemove)) changed = anyTotal = true;

        for (auto& [key, insts] : callGroups) {
            if (insts.size() < 2) continue;
            std::set<BasicBlock*> blockSet;
            for (auto* i : insts) blockSet.insert(i->getParent());
            if (blockSet.size() < 2) continue;
            BasicBlock* L = *blockSet.begin();
            for (auto* bb : blockSet) {
                BasicBlock* a = L, *b = bb;
                while (a != b) {
                    if (!a || !b) { L = nullptr; break; }
                    if (depth[a] < depth[b]) std::swap(a, b);
                    a = dt.getIDom(a);
                }
                L = a;
                if (!L) break;
            }
            if (!L || blockSet.count(L)) continue;
            if (!isAnticipatable(blockSet, L)) continue;
            if (hoistGroup(insts, toRemove)) changed = anyTotal = true;
        }

        for (auto inst : toRemove)
            inst->eraseInst();
    }

    return anyTotal;
}
