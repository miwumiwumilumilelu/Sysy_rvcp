#include "Optimize/High/HighMem2Reg.h"
#include "IR/Type.h"
#include <algorithm>

using namespace sysy;

// Check if a region contains any impure insts(break/continue).
static bool impureRegion(Region* region) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (isa<BreakInst>(inst) || isa<ContinueInst>(inst)) return true;
            if (auto ifInst = dyn_cast<IfInst>(inst)) {
                if (impureRegion(ifInst->getThenRegion())) 
                    return true;
                if (ifInst->getElseRegion() && impureRegion(ifInst->getElseRegion()))
                    return true;
            }
        }
    }
    return false;
}

std::string HighMem2Reg::nextName(const char* prefix) {
    return std::string(prefix) + std::to_string(Counter++);
}

Value* HighMem2Reg::getZero(AllocaInst* ai) {
    Type* ty = ai->getAllocatedType();
    if (ty->isFloat()) return new ConstantFloat(0.0f);
    return new ConstantInt(0);
}

bool HighMem2Reg::isPromotable(AllocaInst* ai) {
    if (ai->getAllocatedType()->isArray()) return false;
    for (auto user : ai->getUsers()) {
        // x = load ai 
        // after HighMem2Reg:
        // x = vals[ai]
        if (isa<LoadInst>(user)) continue;
        // store val, ai
        // after HighMem2Reg:
        // vals[ai] = val
        if (auto st = dyn_cast<StoreInst>(user)) {
            if (st->getOperand(1) == ai) continue;
        }
        return false;
    }
    return true;
}

void HighMem2Reg::collectPromotable(Function* func) {
    Promotable.clear();
    auto* entry = func->getEntryBlock();
    if (!entry) return;

    Region* body = func->getBody();
    std::map<BasicBlock*, int> blockPos;
    {
        int idx = 0;
        for (auto bb : body->getBlocks()) blockPos[bb] = idx++;
    }

    // Handle Array init shortcut which is implemented in IRGen.cpp.  
    // This is a special case.
    std::set<BasicBlock*> flatLoopBlocks;
    for (auto bb : body->getBlocks()) {
        auto& insts = bb->getInstructions();
        if (insts.empty()) continue;
        auto* term = insts.back();
        if (!isa<BranchInst>(term)) continue;
        int CurPos = blockPos.count(bb) ? blockPos.at(bb) : -1;
        for (int i = 0; i < term->getNumOperands(); i++) {
            if (auto* targetBB = dyn_cast<BasicBlock>(term->getOperand(i))) {
                // If targetBB pos is less than or equal to Curbb pos, then Curbb is in a flat back-edge block.
                if (blockPos.count(targetBB) && blockPos.at(targetBB) <= CurPos) {
                    flatLoopBlocks.insert(bb);
                    break;
                }
            }
        }
    }

    for (auto inst : entry->getInstructions()) {
        if (auto ai = dyn_cast<AllocaInst>(inst)) {
            if (!isPromotable(ai)) continue;
            bool bad = false;
            for (auto user : ai->getUsers()) {
                if (auto st = dyn_cast<StoreInst>(user)) {
                    if (st->getOperand(1) == ai && flatLoopBlocks.count(st->getParent())) {
                        bad = true;
                        break;
                    }
                }
            }
            if (!bad) Promotable.insert(ai);
        }
    }

    std::set<AllocaInst*> impureai;
    markBadAllocas(func->getBody(), impureai);
    for (auto ai : impureai) Promotable.erase(ai);
}

void HighMem2Reg::markBadAllocas(Region* region, std::set<AllocaInst*>& bad) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto whileInst = dyn_cast<WhileInst>(inst)) {
                if (impureRegion(whileInst->getBodyRegion()) || impureRegion(whileInst->getCondRegion())) {
                    // All allocas written anywhere inside this while are tainted.
                    findWritten(whileInst->getBodyRegion(), bad);
                    findWritten(whileInst->getCondRegion(), bad);
                }
                // Always recurse to discover nested bad whiles.
                markBadAllocas(whileInst->getBodyRegion(), bad);
                markBadAllocas(whileInst->getCondRegion(), bad);
            } else if (auto ifInst = dyn_cast<IfInst>(inst)) {
                markBadAllocas(ifInst->getThenRegion(), bad);
                if (ifInst->getElseRegion())
                    markBadAllocas(ifInst->getElseRegion(), bad);
            }
        }
    }
}

void HighMem2Reg::findWritten(Region* region, std::set<AllocaInst*>& written) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto store = dyn_cast<StoreInst>(inst)) {
                if (auto ai = dyn_cast<AllocaInst>(store->getOperand(1))) {
                    if (Promotable.count(ai)) written.insert(ai);
                }
            } else if (auto ifInst = dyn_cast<IfInst>(inst)) {
                findWritten(ifInst->getThenRegion(), written);
                if (auto e = ifInst->getElseRegion()) findWritten(e, written);
            } else if (auto whileInst = dyn_cast<WhileInst>(inst)) {
                findWritten(whileInst->getBodyRegion(), written);
            }
        }
    }
}

bool HighMem2Reg::insertFlowInst(Region* region, const std::vector<Value*>& vals) {
    if (region->getBlocks().empty()) {
        new BasicBlock("_flow", region);
    }

    bool inserted = false;
    for (auto bb : region->getBlocks()) {
        auto& insts = bb->getInstructions();
        // Skip already-terminated blocks (BranchInst, ReturnInst, Break, Continue, Flow).
        if (!insts.empty() && insts.back()->isTerminator()) continue;
        new FlowInst(vals, bb);
        inserted = true;
    }
    return inserted;
}

void HighMem2Reg::eraseInst(Instruction* inst) {
    if (inst->getParent()) inst->getParent()->getInstructions().remove(inst);
    inst->replaceAllUsesWith(nullptr);
    for (int i = 0; i < inst->getNumOperands(); i++) inst->setOperand(i, nullptr);
    inst->setParent(nullptr);
    delete inst;
}

void HighMem2Reg::run() {
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;

        collectPromotable(func);
        if (Promotable.empty()) continue;
        ValMap vals;
        processRegion(func->getBody(), vals);

        for (auto ai : Promotable) {
            eraseInst(ai);
        }
    }
}

void HighMem2Reg::processRegion(Region* region, ValMap& vals) {
    for (auto bb : region->getBlocks()) {
        processBlock(bb, vals);
    }
}

void HighMem2Reg::processBlock(BasicBlock* bb, ValMap& vals) {
    std::vector<Instruction*> toRemove;

    // Take a snapshot since processIfInst/WhileInst may insert new instructions.
    std::vector<Instruction*> snap(bb->getInstructions().begin(), bb->getInstructions().end());

    for (auto inst : snap) {
        if (auto ai = dyn_cast<AllocaInst>(inst)) {
            if (Promotable.count(ai)) continue; // Erased by run() after all blocks.
        } else if (auto load = dyn_cast<LoadInst>(inst)) {
            if (auto ai = dyn_cast<AllocaInst>(load->getOperand(0))) {
                if (Promotable.count(ai)) {
                    Value* v = vals.count(ai) ? vals[ai] : getZero(ai);
                    load->replaceAllUsesWith(v);
                    toRemove.push_back(load);
                }
            }
        } else if (auto store = dyn_cast<StoreInst>(inst)) {
            if (auto ai = dyn_cast<AllocaInst>(store->getOperand(1))) {
                if (Promotable.count(ai)) {
                    vals[ai] = store->getOperand(0);
                    toRemove.push_back(store);
                }
            }
        } else if (auto ifInst = dyn_cast<IfInst>(inst)) {
            processIfInst(ifInst, vals);
        } else if (auto whileInst = dyn_cast<WhileInst>(inst)) {
            processWhileInst(whileInst, vals);
        } else if (isa<BreakInst>(inst) || isa<ContinueInst>(inst)) {
            break; // This path escapes the region.
        }
    }

    for (auto inst : toRemove) eraseInst(inst);
}

void HighMem2Reg::processIfInst(IfInst* ifInst, ValMap& vals) {
    ValMap thenVals = vals;
    processRegion(ifInst->getThenRegion(), thenVals);

    ValMap elseVals = vals;
    if (auto elseRegion = ifInst->getElseRegion()) {
        processRegion(elseRegion, elseVals);
    }

    // Determine which allocas need result values (changed in at least one branch).
    // The order of iteration over Promotable must be consistent with FlowInst ordering.
    std::vector<Value*> thenFlow, elseFlow;

    for (auto ai : Promotable) {
        Value* preV = vals.count(ai) ? vals[ai] : getZero(ai);
        Value* thenV = thenVals.count(ai) ? thenVals[ai] : preV;
        Value* elseV = elseVals.count(ai) ? elseVals[ai] : preV;

        if (thenV == preV && elseV == preV) continue;

        auto* rv = ifInst->createResult(ai->getAllocatedType());
        rv->setName(nextName("%if_r"));

        thenFlow.push_back(thenV);
        elseFlow.push_back(elseV);
        vals[ai] = rv;
    }

    if (thenFlow.empty()) return;

    insertFlowInst(ifInst->getThenRegion(), thenFlow);

    if (!ifInst->getElseRegion()) {
        ifInst->addElseRegion(); // Creates empty region.
    }
    insertFlowInst(ifInst->getElseRegion(), elseFlow);
}

void HighMem2Reg::processWhileInst(WhileInst* whileInst, ValMap& vals) {
    // Find which promotable allocas are written in the loop body.
    std::set<AllocaInst*> written;
    findWritten(whileInst->getBodyRegion(), written);
    findWritten(whileInst->getCondRegion(), written);

    // Build ordered list of loop-carried allocas.
    std::vector<AllocaInst*> loopAllocas;
    for (auto ai : Promotable) {
        if (written.count(ai)) loopAllocas.push_back(ai);
    }

    // Create init operands + ResultValues for each loop-carried alloca.
    for (auto ai : loopAllocas) {
        Value* initV = vals.count(ai) ? vals[ai] : getZero(ai);
        whileInst->addOperand(initV);
        auto* rv = whileInst->createResult(ai->getAllocatedType());
        rv->setName(nextName("%while_r"));
        vals[ai] = rv; // Inside loop, ai maps to this result value.
    }

    // Process cond and body with loop-carried values visible.
    ValMap condVals = vals;
    processRegion(whileInst->getCondRegion(), condVals);

    ValMap bodyVals = vals;
    processRegion(whileInst->getBodyRegion(), bodyVals);

    // Insert FlowInst at normal body exit with the loop-carried values.
    if (!loopAllocas.empty()) {
        std::vector<Value*> flowVals;
        for (auto ai : loopAllocas) {
            flowVals.push_back(bodyVals.count(ai) ? bodyVals[ai] : vals[ai]);
        }
        insertFlowInst(whileInst->getBodyRegion(), flowVals);
    }
}
