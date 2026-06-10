#ifndef IRREWRITER_H
#define IRREWRITER_H

#include "IRBuilder.h"
#include <functional>
#include <list>
#include <map>
#include <set>
#include <vector>

namespace sysy {

// IRRewriter extends IRBuilder with structural rewriting primitives for the
// high-level structured IR (before FlattenCFG).
class IRRewriter : public IRBuilder {
public:
    using InstIter = std::list<Instruction*>::iterator;

    // Find inst in bb's instruction list; returns end() if not found.
    static InstIter findInst(BasicBlock* bb, Instruction* inst);

    // Walk region bottom-up, applying fn to each instruction (including nested).
    // Re-runs until no instruction triggers fn. Returns true if any change.
    template <typename Fn>
    static bool rewriteRegion(Region* region, Fn&& fn) {
        if (!region) return false;
        bool changed = false, again = false;
        do {
            again = false;
            for (auto* bb : region->getBlocks()) {
                std::vector<Instruction*> snap(bb->getInstructions().begin(),
                                               bb->getInstructions().end());
                for (auto* inst : snap) {
                    if (!inst || inst->getParent() != bb) continue;
                    if (auto* fi = dyn_cast<ForInst>(inst))
                        again |= rewriteRegion(fi->getBodyRegion(), fn);
                    else if (auto* wi = dyn_cast<WhileInst>(inst)) {
                        again |= rewriteRegion(wi->getCondRegion(), fn);
                        again |= rewriteRegion(wi->getBodyRegion(), fn);
                    } else if (auto* ii = dyn_cast<IfInst>(inst)) {
                        again |= rewriteRegion(ii->getThenRegion(), fn);
                        if (ii->getElseRegion())
                            again |= rewriteRegion(ii->getElseRegion(), fn);
                    }
                    if (fn(inst)) { again = true; break; }
                }
                if (again) break;
            }
            changed |= again;
        } while (again);
        return changed;
    }
    // Erase inst from its parent block and free it.
    static void eraseOp(Instruction* inst);

    // Replace all uses of inst with newVal, then erase inst.
    static void replaceAndErase(Instruction* inst, Value* newVal);

    // Move inst to just before anchor in anchor's parent block.
    static void moveInstBefore(Instruction* inst, Instruction* anchor);

    // Move inst to just after anchor in anchor's parent block.
    static void moveInstAfter(Instruction* inst, Instruction* anchor);

    // Splice every instruction from every block of src immediately before anchor.
    // All instructions are reparented to anchor's block; src is left empty.
    static void inlineRegionBefore(Region* src, Instruction* anchor);

    // Pre-order walk: visit inst before recursing into its nested regions.
    // fn returns false to abort early; walk returns false iff aborted.
    static bool walk(Region* r, const std::function<bool(Instruction*)>& fn);
    static bool walk(Function* f, const std::function<bool(Instruction*)>& fn);

    // Post-order walk: recurse into nested regions before visiting inst.
    static bool walkPost(Region* r, const std::function<bool(Instruction*)>& fn);
    static bool walkPost(Function* f, const std::function<bool(Instruction*)>& fn);

    // --- IR construction helpers ---

    // Create a new BasicBlock in region.
    static BasicBlock* makeBlock(Region* region, const std::string& name);

    // Append inst to bb (sets parent, adds to end of list).
    static void appendInst(BasicBlock* bb, Instruction* inst);

    // Append a FlowInst with given values to bb.
    static FlowInst* appendFlow(BasicBlock* bb, const std::vector<Value*>& vals);

    // Insert inst into bb at iterator pos.
    static void insertInst(BasicBlock* bb,
                           std::list<Instruction*>::iterator pos,
                           Instruction* inst);

    // Create a detached IfInst (not yet parented to any block).
    static IfInst* makeIf(Value* cond, const std::string& name, bool hasElse = false);

    // Create a detached ForInst (not yet parented to any block).
    static ForInst* makeFor(Value* start, Value* stop, Value* step, Value* ivAddr,
                            ICmpInst::CmpOp pred, const std::string& name);

    // Build min(a,b) or max(a,b) as a constant fold or inline IfInst.
    static Value* buildMinOrMax(Value* a, Value* b, bool wantMin,
                                BasicBlock* parentBB,
                                std::list<Instruction*>::iterator pos);

    // --- Structural transformations ---

    // Remove instructions after the first terminating-flow instruction in bb.
    static void pruneAfterTerminatingFlow(BasicBlock* bb);

    // Replace a constant-condition IfInst with the selected branch's body.
    // Returns false if the branch is not a single basic block.
    static bool inlineSelectedBranch(IfInst* guard, bool takeThen);

    // Clone prefix + selectedRegion (optional) + suffix (optional) into a new
    // "for.body" BasicBlock inside targetRegion.  Returns false on failure.
    static bool cloneForBody(const std::vector<Instruction*>& prefix,
                             Region* selectedRegion,
                             const std::vector<Instruction*>& suffix,
                             bool includeSuffix,
                             Region* targetRegion,
                             std::map<Value*, Value*>& vmap);
};

} // namespace sysy
#endif
