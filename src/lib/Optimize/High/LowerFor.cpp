#include "../../../include/Optimize/High/LowerFor.h"
#include "../../../include/IR/IRRewriter.h"
#include <algorithm>
#include <vector>

using namespace sysy;

// Collect all ContinueInst in region recursively through IfInst (but not WhileInst/ForInst).
static void collectContinues(Region* region, std::vector<ContinueInst*>& out) {
    if (!region) return;
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            if (auto* ci = dyn_cast<ContinueInst>(inst))
                out.push_back(ci);
            else if (auto* ii = dyn_cast<IfInst>(inst)) {
                collectContinues(ii->getThenRegion(), out);
                if (ii->getElseRegion())
                    collectContinues(ii->getElseRegion(), out);
            }
            // ForInst/WhileInst: their continues only affect inner loops, skip.
        }
    }
}

bool LowerFor::run() {
    bool any = false;
    for (auto* f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool LowerFor::runFunc(Function* f) {
    if (!f || f->getBody()->getBlocks().empty()) return false;
    return processRegion(f->getBody());
}

bool LowerFor::processRegion(Region* region) {
    return IRRewriter::rewriteRegion(region, [&](Instruction* inst) {
        auto* fi = dyn_cast<ForInst>(inst);
        return fi && processFor(fi);
    });
}

bool LowerFor::processFor(ForInst* fi) {
    auto* parent = fi->getParent();
    if (!parent) return false;
    auto& insts = parent->getInstructions();
    auto pos = IRRewriter::findInst(parent, fi);
    if (pos == insts.end()) return false;

    Value* ivAddr = fi->getIVAddr();
    Value* start  = fi->getStart();
    Value* stop   = fi->getStop();
    Value* step   = fi->getStep();
    auto pred     = fi->getPred();

    // Emit store(ivAddr, start)  before the while.
    auto* initStore = new StoreInst(start, ivAddr, nullptr);
    initStore->setName("for.init");
    initStore->setParent(parent);
    insts.insert(pos, initStore);

    // Create WhileInst.
    auto* wi = new WhileInst(nullptr);
    wi->setName(fi->getName().empty() ? "for.while" : fi->getName() + ".wh");
    wi->setParent(parent);

    // Build condRegion: single block with ICmpInst(pred, load(ivAddr), stop).
    auto* condBB = new BasicBlock("for.cond", wi->getCondRegion());
    auto* ivLoad = new LoadInst(ivAddr, nullptr);
    ivLoad->setName("for.iv");
    ivLoad->setParent(condBB);
    condBB->getInstructions().push_back(ivLoad);
    auto* cmp = new ICmpInst(pred, ivLoad, stop, nullptr);
    cmp->setName("for.cmp");
    cmp->setParent(condBB);
    condBB->getInstructions().push_back(cmp);

    // Build bodyRegion: move ForInst's body block into wi's body, then append IV update.
    auto* fiBB = fi->getBodyRegion()->getEntryBlock();
    if (fiBB) {
        // Re-parent all instructions in fiBB to a new body block in wi.
        auto* wiBB = new BasicBlock("for.body", wi->getBodyRegion());
        for (auto* inst : fiBB->getInstructions()) {
            inst->setParent(wiBB);
            wiBB->getInstructions().push_back(inst);
        }
        fiBB->getInstructions().clear();

        // Move any extra blocks (from multi-block ForInst body created by &&/|| short-circuit)
        // into the new while body region. Without this they become orphaned when ForInst is erased.
        {
            std::vector<BasicBlock*> extra;
            for (auto* bb : fi->getBodyRegion()->getBlocks())
                if (bb != fiBB) extra.push_back(bb);
            for (auto* bb : extra) {
                fi->getBodyRegion()->removeBlock(bb);
                wi->getBodyRegion()->addBlock(bb);
                bb->setParent(wi->getBodyRegion());
            }
        }

        // After moving fiBB's instructions to wiBB, any phi in the extra blocks that
        // recorded fiBB as an incoming predecessor must now reference wiBB.
        for (auto* bb : wi->getBodyRegion()->getBlocks()) {
            if (bb == wiBB) continue;
            for (auto* inst : bb->getInstructions()) {
                auto* phi = dyn_cast<PhiInst>(inst);
                if (!phi) break;
                for (int i = 1; i < phi->getNumOperands(); i += 2)
                    if (phi->getOperand(i) == fiBB)
                        phi->setOperand(i, wiBB);
            }
        }

        // Find the tail block: the last block in wiBodyRegion that does NOT end with a
        // BranchInst (i.e. the block where the IV update should go).
        // For single-block bodies this is wiBB itself.
        // For multi-block bodies (&&/|| short-circuit) it is the merge block at the end
        // of the chain, which has no explicit branch terminator.
        BasicBlock* tailBB = wiBB;
        for (auto* bb : wi->getBodyRegion()->getBlocks()) {
            if (bb == wiBB) continue;
            if (bb->getInstructions().empty()) continue;
            if (!dyn_cast<BranchInst>(bb->getInstructions().back()))
                tailBB = bb;
        }

        // For each ContinueInst nested in the body, insert the IV update before it
        // so that continue correctly increments the IV (just like end-of-body does).
        {
            std::vector<ContinueInst*> conts;
            collectContinues(wi->getBodyRegion(), conts);
            for (auto* cont : conts) {
                auto* contBB = cont->getParent();
                auto& contInsts = contBB->getInstructions();
                auto it = std::find(contInsts.begin(), contInsts.end(),
                                    static_cast<Instruction*>(cont));
                auto* ld = new LoadInst(ivAddr, nullptr);
                ld->setName("for.iv.inc.c");
                ld->setParent(contBB);
                contInsts.insert(it, ld);
                auto* add = new BinaryInst(Instruction::Add, ld, step, nullptr);
                add->setName("for.iv.next.c");
                add->setParent(contBB);
                contInsts.insert(it, add);
                auto* st = new StoreInst(add, ivAddr, nullptr);
                st->setName("for.iv.store.c");
                st->setParent(contBB);
                contInsts.insert(it, st);
            }
        }

        // Append IV update at the tail of the body chain.
        auto* ivLoad2 = new LoadInst(ivAddr, nullptr);
        ivLoad2->setName("for.iv.inc");
        ivLoad2->setParent(tailBB);
        tailBB->getInstructions().push_back(ivLoad2);
        auto* inc = new BinaryInst(Instruction::Add, ivLoad2, step, nullptr);
        inc->setName("for.iv.next");
        inc->setParent(tailBB);
        tailBB->getInstructions().push_back(inc);
        auto* updStore = new StoreInst(inc, ivAddr, nullptr);
        updStore->setName("for.iv.store");
        updStore->setParent(tailBB);
        tailBB->getInstructions().push_back(updStore);
    }

    // Insert WhileInst in place of ForInst.
    insts.insert(pos, wi);
    fi->replaceAllUsesWith(nullptr);
    fi->eraseInst();

    return true;
}
