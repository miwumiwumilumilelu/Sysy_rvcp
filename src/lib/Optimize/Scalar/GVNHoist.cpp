#include "Optimize/Scalar/GVNHoist.h"
#include "Optimize/Scalar/ExprKey.h"
#include "Optimize/Analysis/Dominators.h"
#include "IR/Instruction.h"
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace sysy;

// Safe to execute speculatively (no side effects, no div-by-zero risk).
static bool isSafe(Instruction* inst) {
    auto op = inst->getOpID();
    return isa<BinaryInst>(inst) || isa<ICmpInst>(inst) || isa<FCmpInst>(inst) || 
            isa<CastInst>(inst) || isa<GetElementPtrInst>(inst);
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
    for (auto bb : f->getBody()->getBlocks()) {
        domCh[bb] = {};
        if (auto* idom = dt.getIDom(bb)) domCh[idom].push_back(bb);
    }
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

    // fix-point iteration
    bool anyTotal = false;
    bool changed = true;
    while (changed) {
        changed = false;

        // Rebuild groups each iteration since instruction positions changed.
        std::map<ExprKey, std::vector<Instruction*>> groups;
        for (auto bb : f->getBody()->getBlocks()) {
            for (auto inst : bb->getInstructions()) {
                if (!isSafe(inst)) continue;
                ExprKey k = makeExprKey(inst);
                if (k == ExprKey{0, 0, 0}) continue;
                groups[k].push_back(inst);
            }
        }

        std::vector<Instruction*> toRemove;

        for (auto& [key, insts] : groups) {
            if (insts.size() < 2) continue;

            std::set<BasicBlock*> blockSet;
            for (auto inst : insts) blockSet.insert(inst->getParent());
            
            // All insts from groups be in the same block, 
            // thus CSE handles. Skip.
            if (blockSet.size() < 2) continue;

            // Find the closest common ancestor of all these blocks through LCA.
            BasicBlock* L = *blockSet.begin();
            for (auto bb : blockSet) L = lca(L, bb);
            if (!L) continue;

            // If the LCA itself is the block of an instance, 
            // it means there is an instance that dominates all other instances,
            // thus GVN handles. Skip.
            if (blockSet.count(L)) continue;

            if (!operandsAvailAt(insts[0], L)) continue;

            auto* hoisted = cloneToBlock(insts[0], L);
            if (!hoisted) continue;
            for (auto inst : insts) {
                inst->replaceAllUsesWith(hoisted);
                toRemove.push_back(inst);
            }
            changed = anyTotal = true;
        }

        for (auto inst : toRemove)
            inst->getParent()->getInstructions().remove(inst);
    }

    return anyTotal;
}
