#include "Optimize/CFG/FlattenCFG.h"
#include "IR/IRBuilder.h"
#include "IR/Instruction.h"
#include <iostream>
#include <algorithm>
#include <vector>

using namespace sysy;

void FlattenCFG::run() {
    for (auto func : TheModule->getFunctions()) {
        flattenRegion(func->getBody(), nullptr, nullptr);    

        int bbCounter = 0;
        for (auto bb : func->getBody()->getBlocks()) {
            bb->setName("bb" + std::to_string(bbCounter++));
        }
    }
}

void moveBlocksFromRegion(Region* src, Region* dest) {
    auto& srcBlocks = src->getBlocks();
    auto& destBlocks = dest->getBlocks();
    for (auto bb : srcBlocks) bb->setParent(dest);
    destBlocks.splice(destBlocks.end(), srcBlocks);
}

void FlattenCFG::flattenRegion(Region* region, BasicBlock* loopHeader, BasicBlock* loopExit) {
    auto& blocks = region->getBlocks();

    for (auto it = blocks.begin(); it != blocks.end(); ) {
        BasicBlock* bb = *it;
        
        Instruction* highLevelInst = nullptr;
        for (auto inst : bb->getInstructions()) {
            if (inst->getOpID() == Instruction::If || 
                inst->getOpID() == Instruction::While ||
                inst->getOpID() == Instruction::Break ||
                inst->getOpID() == Instruction::Continue) {
                highLevelInst = inst;
                break;
            }
        }

        if (!highLevelInst) {
            it++;
            continue;
        }

        // When encountering an advanced instruction, 
        // the instructions after that instruction must be split into a new Merge Block.
        BasicBlock* mergeBB = new BasicBlock(bb->getName() + "_cont", region);

        auto &insts = bb->getInstructions();
        auto instIt = std::find(insts.begin(), insts.end(), highLevelInst);

        if (instIt != insts.end()) {
            auto nextIt = std::next(instIt);
            mergeBB->getInstructions().splice(mergeBB->getInstructions().begin(), insts, nextIt, insts.end());

            for (auto inst : mergeBB->getInstructions()) {
                inst->setParent(mergeBB);
            }
        }

        // After splitting bb into bb+mergeBB, any phi [val, bb] in the region
        // that was set up by code now in mergeBB's subgraph needs to be redirected
        // to mergeBB, since bb will get a new terminator (from handleIf/handleWhile)
        // that won't reach those phi blocks anymore.
        for (auto blk : region->getBlocks()) {
            for (auto inst : blk->getInstructions()) {
                if (auto phi = dyn_cast<PhiInst>(inst)) {
                    for (int i = 0; i < phi->getNumOperands(); i += 2) {
                        if (phi->getOperand(i+1) == bb) {
                            phi->setOperand(i+1, mergeBB);
                        }
                    }
                } else {
                    break;
                }
            }
        }

        if (auto ifInst = dyn_cast<IfInst>(highLevelInst)) {
            handleIf(ifInst, bb, mergeBB, loopHeader, loopExit);
        } else if (auto whileInst = dyn_cast<WhileInst>(highLevelInst)) {
            handleWhile(whileInst, bb, mergeBB);
        } else if (auto brk = dyn_cast<BreakInst>(highLevelInst)) {
            // Handle break: jump to loopExit
            if (loopExit) {
                builder.SetInsertPoint(bb);
                builder.CreateBr(loopExit);
                bb->getInstructions().remove(brk);
            } else {
                std::cerr << "Error: Break outside loop" << std::endl;
            }
        } else if (auto cont = dyn_cast<ContinueInst>(highLevelInst)) {
            // Handle continue: jump to loopHeader
            if (loopHeader) {
                builder.SetInsertPoint(bb);
                builder.CreateBr(loopHeader);
                bb->getInstructions().remove(cont);
            } else {
                std::cerr << "Error: Continue outside loop" << std::endl;
            }
        }
        // The loop is naturally processed to mergeBB (because it is added to the linked list).
    }
}

// Collect all FlowInst blocks in a region; replace each FlowInst with br targetBB.
// Returns a list of (block, FlowInst-operand-values) for building phi nodes.
static std::vector<std::pair<BasicBlock*, std::vector<Value*>>>
collectAndReplaceFlows(Region* region, BasicBlock* targetBB, IRBuilder& builder) {
    std::vector<std::pair<BasicBlock*, std::vector<Value*>>> result;
    for (auto bb : region->getBlocks()) {
        if (bb->getInstructions().empty()) continue;
        auto* back = bb->getInstructions().back();
        if (auto* flow = dyn_cast<FlowInst>(back)) {
            std::vector<Value*> vals;
            for (int i = 0; i < flow->getNumOperands(); i++)
                vals.push_back(flow->getOperand(i));
            result.push_back({bb, vals});
            // Remove FlowInst and replace with br targetBB.
            for (int i = 0; i < flow->getNumOperands(); i++) flow->setOperand(i, nullptr);
            flow->setParent(nullptr);
            bb->getInstructions().remove(flow);
            delete flow;
            builder.SetInsertPoint(bb);
            builder.CreateBr(targetBB);
        }
    }
    return result;
}

void FlattenCFG::handleIf(IfInst* inst, BasicBlock* currentBB, BasicBlock* mergeBB, BasicBlock* loopHeader, BasicBlock* loopExit) {
    Region* parentRegion = currentBB->getParent();

    Region* thenRegion = inst->getThenRegion();
    flattenRegion(thenRegion, loopHeader, loopExit);

    BasicBlock* thenEntry = thenRegion->getEntryBlock();

    // Handle FlowInst blocks in then region (produced by HighMem2Reg).
    auto thenFlows = collectAndReplaceFlows(thenRegion, mergeBB, builder);

    for (auto bb : thenRegion->getBlocks()) {
        if (bb->getInstructions().empty() || !bb->getInstructions().back()->isTerminator()) {
            builder.SetInsertPoint(bb);
            builder.CreateBr(mergeBB);
        }
    }

    // If there is no Else, default jump to Merge.
    BasicBlock* elseEntry = mergeBB;
    std::vector<std::pair<BasicBlock*, std::vector<Value*>>> elseFlows;
    if (auto elseRegion = inst->getElseRegion()) {
        flattenRegion(elseRegion, loopHeader, loopExit);
        elseEntry = elseRegion->getEntryBlock();

        elseFlows = collectAndReplaceFlows(elseRegion, mergeBB, builder);

        for (auto bb : elseRegion->getBlocks()) {
            if (bb->getInstructions().empty() || !bb->getInstructions().back()->isTerminator()) {
                builder.SetInsertPoint(bb);
                builder.CreateBr(mergeBB);
            }
        }
        moveBlocksFromRegion(elseRegion, parentRegion);
    }

    builder.SetInsertPoint(currentBB);
    builder.Create<BranchInst>(inst->getOperand(0), thenEntry, elseEntry);

    // Build phi nodes at mergeBB for each IfInst result (from HighMem2Reg).
    if (inst->getNumResults() > 0 && (!thenFlows.empty() || !elseFlows.empty())) {
        unsigned numResults = inst->getNumResults();
        for (unsigned i = 0; i < numResults; i++) {
            auto* rv = inst->getResult(i);
            auto* phi = new PhiInst(rv->getType(), nullptr);
            phi->setParent(mergeBB);
            mergeBB->getInstructions().push_front(phi);
            phi->setName(rv->getName());

            for (auto& [bb, vals] : thenFlows) {
                if (i < vals.size()) phi->addIncoming(vals[i], bb);
            }
            for (auto& [bb, vals] : elseFlows) {
                if (i < vals.size()) phi->addIncoming(vals[i], bb);
            }

            rv->replaceAllUsesWith(phi);
        }
    }

    moveBlocksFromRegion(thenRegion, parentRegion);
    currentBB->getInstructions().remove(inst);
}

void FlattenCFG::handleWhile(WhileInst* inst, BasicBlock* currentBB, BasicBlock* mergeBB) {
    Region* parentRegion = currentBB->getParent();
    Region* condRegion = inst->getCondRegion();
    Region* bodyRegion = inst->getBodyRegion();

    BasicBlock* condEntry = condRegion->getEntryBlock();

    // We need phi nodes at condEntry to merge the init values (from currentBB),
    // with the loop-back values (from the body FlowInst).
    // Build them before flattening so they appear at the front of condEntry.
    unsigned numResults = inst->getNumResults();
    std::vector<PhiInst*> loopPhis;
    for (unsigned i = 0; i < numResults; i++) {
        auto* rv = inst->getResult(i);
        auto* phi = new PhiInst(rv->getType(), nullptr);
        phi->setParent(condEntry);
        condEntry->getInstructions().push_front(phi);
        phi->setName(rv->getName());
        // Init incoming from currentBB (the block before the loop).
        phi->addIncoming(inst->getOperand(i), currentBB);
        rv->replaceAllUsesWith(phi);
        loopPhis.push_back(phi);
    }

    flattenRegion(condRegion, condEntry, mergeBB);
    flattenRegion(bodyRegion, condEntry, mergeBB);

    // Collect FlowInst blocks in the body region and add loop-back phis.
    auto bodyFlows = collectAndReplaceFlows(bodyRegion, condEntry, builder);
    for (auto& [bb, vals] : bodyFlows) {
        for (unsigned i = 0; i < loopPhis.size() && i < vals.size(); i++) {
            loopPhis[i]->addIncoming(vals[i], bb);
        }
    }

    // When short-circuiting evaluation, there is a possibility that
    // the merge block may be generated in the middle due to priority reasons.
    BasicBlock* condExit = nullptr;
    for (auto bb : condRegion->getBlocks()) {
        if (bb->getInstructions().empty() || !bb->getInstructions().back()->isTerminator()) {
            condExit = bb;
            break;
        }
    }
    if (!condExit) condExit = condRegion->getBlocks().back();

    BasicBlock* bodyEntry = bodyRegion->getEntryBlock();

    builder.SetInsertPoint(currentBB);
    builder.CreateBr(condEntry);

    Value* condVal = nullptr;
    for (auto it = condExit->getInstructions().rbegin();
         it != condExit->getInstructions().rend(); ++it) {
        if ((*it)->isTerminator() || isa<PhiInst>(*it)) continue;
        condVal = *it;
        break;
    }

    builder.SetInsertPoint(condExit);
    builder.Create<BranchInst>(condVal, bodyEntry, mergeBB);

    for (auto bb : bodyRegion->getBlocks()) {
        if (bb->getInstructions().empty() || !bb->getInstructions().back()->isTerminator()) {
            builder.SetInsertPoint(bb);
            builder.CreateBr(condEntry);
        }
    }

    moveBlocksFromRegion(condRegion, parentRegion);
    moveBlocksFromRegion(bodyRegion, parentRegion);
    currentBB->getInstructions().remove(inst);
}