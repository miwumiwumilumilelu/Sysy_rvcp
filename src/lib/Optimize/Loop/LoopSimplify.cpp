#include "../../../include/Optimize/Loop/LoopSimplify.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/IR/Instruction.h"
#include <algorithm>
#include <cassert>
#include <functional>

using namespace sysy;

static int LoopSimplifyInstID = 0;
static int LoopSimplifyBBID = 0;

static std::string InstName(const std::string& seed) {
    // %seed.ls0\ %seed.ls1\ ...
    if (!seed.empty())
        return seed + ".ls" + std::to_string(LoopSimplifyInstID++);
    // %ls0\ %ls1\ ...
    return "%ls" + std::to_string(LoopSimplifyInstID++);
}

static std::string BBName(const std::string& seed) {
    if (!seed.empty())
        return seed + ".ls" + std::to_string(LoopSimplifyBBID++);
    return "lsbb" + std::to_string(LoopSimplifyBBID++);
}

static void assignName(Instruction* inst, const std::string& seed = "") {
    if (!inst || inst->getType()->isVoid()) return;
    inst->setName(InstName(seed));
}

bool LoopSimplify::buildPrehBB(Loop* L, Dominators& dt) {
    assert(L && "buildPrehBB expects a valid loop");
    if (L->pre) return false;

    std::vector<BasicBlock*> ext;
    for (auto* pred : dt.getPredecessors(L->head)) {
        if (!L->has(pred))
            ext.push_back(pred);
    }

    assert(!ext.empty() && "natural loop header must have an outside predecessor");

    Region* region = L->head->getParent();
    auto* preheaderBB = new BasicBlock("pre_" + L->head->getName(), region);
    auto& blist = region->getBlocks();
    auto itHead = std::find(blist.begin(), blist.end(), L->head);
    blist.splice(itHead, blist, std::prev(blist.end()));

    for (auto* ep : ext) {
        auto* term = ep->getInstructions().empty() ? nullptr : ep->getInstructions().back();
        auto* br = dyn_cast<BranchInst>(term);
        assert(br && "external predecessor must end with a branch");
        br->replaceSuccessor(L->head, preheaderBB);
    }

    auto& preInsts = preheaderBB->getInstructions();
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;

        std::vector<std::pair<Value*, BasicBlock*>> forwarded;
        for (int i = 0; i < phi->getNumOperands(); i += 2) {
            auto* val = phi->getOperand(i);
            auto* from = cast<BasicBlock>(phi->getOperand(i + 1));
            if (std::find(ext.begin(), ext.end(), from) != ext.end())
                forwarded.push_back({val, from});
        }

        if (forwarded.empty()) continue;

        Value* merged = forwarded[0].first;
        if (forwarded.size() > 1) {
            auto* prePhi = new PhiInst(phi->getType(), nullptr);
            prePhi->setName(phi->getName() + ".ph");
            for (auto& [val, from] : forwarded)
                prePhi->addIncoming(val, from);
            prePhi->setParent(preheaderBB);
            preInsts.push_back(prePhi);
            merged = prePhi;
        }

        for (auto& [unused, from] : forwarded)
            phi->removeIncomingByBlock(from);
        phi->addIncoming(merged, preheaderBB);
    }

    new BranchInst(L->head, preheaderBB);
    L->pre = preheaderBB;
    return true;
}

BasicBlock* LoopSimplify::mergeLatches(Loop* L) {
    assert(L && "mergeLatches expects a valid loop");
    assert(!L->latches.empty() && "mergeLatches expects at least one latch");
    if (L->latches.size() == 1) {
        L->latch = L->latches[0];
        return L->latch;
    }

    std::vector<PhiInst*> hphis;
    for (auto* inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        hphis.push_back(phi);
    }

    Region* region = L->head->getParent();
    auto* nl = new BasicBlock("latch_merge_" + L->head->getName(), region);
    L->blocks.push_back(nl);
    L->blockSet.insert(nl);

    for (auto* hp : hphis) {
        auto* fwd = new PhiInst(hp->getType(), nullptr);
        fwd->setParent(nl);
        assignName(fwd, hp->getName());
        for (auto* lat : L->latches) {
            Value* val = nullptr;
            for (int k = 0; k < hp->getNumOperands(); k += 2) {
                if (hp->getOperand(k + 1) == lat) {
                    val = hp->getOperand(k);
                    break;
                }
            }
            assert(val && "header phi must have an incoming for every latch");
            fwd->addIncoming(val, lat);
        }
        nl->getInstructions().push_back(fwd);
        for (auto* lat : L->latches)
            hp->removeIncomingByBlock(lat);
        hp->addIncoming(fwd, nl);
    }

    new BranchInst(L->head, nl);

    for (auto* lat : L->latches) {
        auto* br = dyn_cast<BranchInst>(lat->getInstructions().empty() ? nullptr : lat->getInstructions().back());
        if (!br) assert(false && "Latch block must end with a branch");
        for (int k = 0; k < br->getNumOperands(); ++k) {
            if (dyn_cast<BasicBlock>(br->getOperand(k)) == L->head)
                br->setOperand(k, nl);
        }
    }

    L->latches = {nl};
    L->latch = nl;
    return nl;
}

bool LoopSimplify::dedicateExits(Loop* L, Dominators& dt) {
    bool changed = false;

    // Snapshot
    std::vector<BasicBlock*> exits = L->exits;

    for (auto* exitBB : exits) {
        std::vector<BasicBlock*> loopPreds, nonLoopPreds;
        for (auto* pred : dt.getPredecessors(exitBB)) {
            if (L->has(pred)) 
                loopPreds.push_back(pred);
            else 
                nonLoopPreds.push_back(pred);
        }

        // Already dedicated (all preds from loop), or exitBB is unreachable / not a real exit.
        if (nonLoopPreds.empty() || loopPreds.empty()) continue;

        Region* region = exitBB->getParent();
        auto* ded = new BasicBlock(BBName("ded_exit_" + exitBB->getName()), region);

        // Place ded immediately before exitBB in the block list.
        // ... , ded, exitBB, ...
        auto& blist = region->getBlocks();
        auto itExit = std::find(blist.begin(), blist.end(), exitBB);
        blist.splice(itExit, blist, std::prev(blist.end()));

        for (auto* inst : exitBB->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;

            std::vector<std::pair<Value*, BasicBlock*>> forwarded;
            for (int i = 0; i < phi->getNumOperands(); i += 2) {
                auto* val  = phi->getOperand(i);
                auto* from = cast<BasicBlock>(phi->getOperand(i + 1));
                if (std::find(loopPreds.begin(), loopPreds.end(), from) != loopPreds.end())
                    forwarded.push_back({val, from});
            }
            if (forwarded.empty()) continue;

            Value* merged = forwarded[0].first;
            if (forwarded.size() > 1) {
                auto* fwd = new PhiInst(phi->getType(), nullptr);
                fwd->setName(phi->getName() + ".ded");
                fwd->setParent(ded);
                for (auto& [v, b] : forwarded)
                    fwd->addIncoming(v, b);
                ded->getInstructions().push_back(fwd);
                merged = fwd;
            }

            for (auto& [unused, b] : forwarded)
                phi->removeIncomingByBlock(b);
            phi->addIncoming(merged, ded);
        }

        // loopPreds -> exitBB 
        //
        // becomes:
        //
        // loopPreds -> ded 
        for (auto* lp : loopPreds) {
            auto& insts = lp->getInstructions();
            if (insts.empty()) continue;
            if (auto* br = dyn_cast<BranchInst>(insts.back()))
                br->replaceSuccessor(exitBB, ded);
        }

        // ded -> exitBB
        new BranchInst(exitBB, ded);

        changed = true;
    }

    return changed;
}

bool LoopSimplify::runOnFunction(Function* f) {
    if (!f || f->getBody()->getBlocks().empty()) return false;

    bool anychanged = false;
    bool changed;
    do {
        changed = false;
        Dominators dt(f);
        dt.run();
        LoopInfo li(f, dt);

        std::function<bool(Loop*)> visit = [&](Loop* L) -> bool {
            // Post-order: inner loops first.
            for (auto* sub : L->sub) {
                if (visit(sub)) return true;
            }

            assert(!L->latches.empty() && "LoopSimplify expects every loop to have at least one latch");

            // build preheader first.
            if (buildPrehBB(L, dt)) return true;

            // mergeLatches to build sigle latch.
            if (L->latches.size() > 1) {
                mergeLatches(L);
                return true;
            }

            // dedicated exit blocks.
            if (dedicateExits(L, dt)) return true;

            return false;
        };

        for (auto* top : li.tops()) {
            if (visit(top)) {
                changed = true;
                anychanged = true;
                break;
            }
        }
    } while (changed);

    return anychanged;
}

bool LoopSimplify::run() {
    bool changed = false;
    for (auto* f : M->getFunctions())
        changed |= runOnFunction(f);
    return changed;
}
