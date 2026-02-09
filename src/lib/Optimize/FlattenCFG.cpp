#include "Optimize/FlattenCFG.h"
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

void FlattenCFG::handleIf(IfInst* inst, BasicBlock* currentBB, BasicBlock* mergeBB, BasicBlock* loopHeader, BasicBlock* loopExit) {
    Region* parentRegion = currentBB->getParent();

    Region* thenRegion = inst->getThenRegion();
    flattenRegion(thenRegion, loopHeader, loopExit);

    BasicBlock* thenEntry = thenRegion->getEntryBlock();

    for (auto bb : thenRegion->getBlocks()) {
        if (bb->getInstructions().empty() || !bb->getInstructions().back()->isTerminator()) {
            builder.SetInsertPoint(bb);
            builder.CreateBr(mergeBB);
        }
    }

    // If there is no Else, default jump to Merge.
    BasicBlock* elseEntry = mergeBB;
    if (auto elseRegion = inst->getElseRegion()) {
        flattenRegion(elseRegion, loopHeader, loopExit);
        elseEntry = elseRegion->getEntryBlock();

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

    moveBlocksFromRegion(thenRegion, parentRegion);
    currentBB->getInstructions().remove(inst);
}

void FlattenCFG::handleWhile(WhileInst* inst, BasicBlock* currentBB, BasicBlock* mergeBB) {
    Region* parentRegion = currentBB->getParent();
    Region* condRegion = inst->getCondRegion();
    Region* bodyRegion = inst->getBodyRegion();

    BasicBlock* condEntry = condRegion->getEntryBlock();

    flattenRegion(condRegion, condEntry, mergeBB);
    flattenRegion(bodyRegion, condEntry, mergeBB);

    BasicBlock* condExit = condRegion->getBlocks().back();
    BasicBlock* bodyEntry = bodyRegion->getEntryBlock();

    builder.SetInsertPoint(currentBB);
    builder.CreateBr(condEntry);

    Value* condVal = nullptr;
    if (!condExit->getInstructions().empty()) {
        Instruction* last = condExit->getInstructions().back();
        if (!last->isTerminator()) {
            condVal = last;
        } else if (condExit->getInstructions().size() >= 2) {
            auto it = condExit->getInstructions().end();
            condVal = *(--(--it));
        }
    }

    // If the condition is empty, we treat it as "while (true)".
    if (!condVal) condVal = new ConstantInt(1);

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