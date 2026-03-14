#include "Optimize/HighLICM.h"
#include <algorithm>

using namespace sysy;

// Strip GEP chain to find the base pointer of a memory access.
static Value* getBase(Value* ptr) {
    while (auto gep = dyn_cast<GetElementPtrInst>(ptr))
        ptr = gep->getOperand(0);
    return ptr;
}

void HighLICM::run() {
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;
        processRegion(func->getBody());
    }
}

void HighLICM::processRegion(Region* region) {
    for (auto bb : region->getBlocks()) {
        std::vector<Instruction*> snap(bb->getInstructions().begin(), bb->getInstructions().end());
        for (auto inst : snap) {
            if (auto whileInst = dyn_cast<WhileInst>(inst)) {
                processRegion(whileInst->getCondRegion());
                processRegion(whileInst->getBodyRegion());
                processWhile(whileInst);
            } else if (auto ifInst = dyn_cast<IfInst>(inst)) {
                processRegion(ifInst->getThenRegion());
                if (ifInst->getElseRegion())
                    processRegion(ifInst->getElseRegion());
            }
        }
    }
}

void HighLICM::collectDefs(Region* region, std::set<Value*>& defs) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            defs.insert(inst);
            if (auto whileInst = dyn_cast<WhileInst>(inst)) {
                for (auto rv : whileInst->getResults()) defs.insert(rv);
                collectDefs(whileInst->getCondRegion(), defs);
                collectDefs(whileInst->getBodyRegion(), defs);
            } else if (auto ifInst = dyn_cast<IfInst>(inst)) {
                for (auto rv : ifInst->getResults()) defs.insert(rv);
                collectDefs(ifInst->getThenRegion(), defs);
                if (ifInst->getElseRegion())
                    collectDefs(ifInst->getElseRegion(), defs);
            }
        }
    }
}

void HighLICM::scanLoop(Region* region, std::set<Value*>& storeBases, bool& hasCall) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto st = dyn_cast<StoreInst>(inst)) {
                storeBases.insert(getBase(st->getOperand(1)));
            } else if (isa<CallInst>(inst)) {
                hasCall = true;
            } else if (auto wi = dyn_cast<WhileInst>(inst)) {
                scanLoop(wi->getCondRegion(), storeBases, hasCall);
                scanLoop(wi->getBodyRegion(), storeBases, hasCall);
            } else if (auto ii = dyn_cast<IfInst>(inst)) {
                scanLoop(ii->getThenRegion(), storeBases, hasCall);
                if (ii->getElseRegion())
                    scanLoop(ii->getElseRegion(), storeBases, hasCall);
            }
        }
    }
}

bool HighLICM::isHoistable(Instruction* inst, 
                            const std::set<Value*>& loopDefs,
                            const std::set<Value*>& storeBases, 
                            bool loopHasCall
                        ) {
    switch (inst->getOpID()) {
        case Instruction::Add: case Instruction::Sub:
        case Instruction::Mul: case Instruction::Div: case Instruction::Mod:
        case Instruction::FAdd: case Instruction::FSub:
        case Instruction::FMul: case Instruction::FDiv:
        case Instruction::ICmp: case Instruction::FCmp:
        case Instruction::SIToFP: case Instruction::FPToSI:
        case Instruction::GetElementPtr:
            break;
        case Instruction::Load: {
            // If any call exists in the loop, the callee may write memory, so skip.
            if (loopHasCall) return false;
            // Safe only if the load's base is not written by any store in the loop.
            Value* base = getBase(inst->getOperand(0));
            if (storeBases.count(base)) return false;
            break;
        }
        default:
            return false;
    }
    // All operands must be defined outside the loop.
    for (int i = 0; i < inst->getNumOperands(); i++) {
        Value* op = inst->getOperand(i);
        if (op && loopDefs.count(op)) return false;
    }
    return true;
}

void HighLICM::processWhile(WhileInst* wi) {
    BasicBlock* parentBB = wi->getParent();
    if (!parentBB) return;

    std::set<Value*> loopDefs;
    collectDefs(wi->getCondRegion(), loopDefs);
    collectDefs(wi->getBodyRegion(), loopDefs);
    for (auto rv : wi->getResults()) loopDefs.insert(rv);

    std::set<Value*> storeBases;
    bool loopHasCall = false;
    scanLoop(wi->getBodyRegion(), storeBases, loopHasCall);
    scanLoop(wi->getCondRegion(), storeBases, loopHasCall);

    auto& parentInsts = parentBB->getInstructions();
    auto insertPos = std::find(parentInsts.begin(), parentInsts.end(), static_cast<Instruction*>(wi));

    bool changed = true;
    while (changed) {
        changed = false;
        for (Region* region : {wi->getBodyRegion(), wi->getCondRegion()}) {
            for (auto bb : region->getBlocks()) {
                auto& insts = bb->getInstructions();
                for (auto it = insts.begin(); it != insts.end(); ) {
                    Instruction* inst = *it;
                    if (isHoistable(inst, loopDefs, storeBases, loopHasCall)) {
                        it = insts.erase(it);
                        loopDefs.erase(inst);
                        inst->setParent(parentBB);
                        parentInsts.insert(insertPos, inst);
                        changed = true;
                    } else {
                        ++it;
                    }
                }
            }
        }
    }
}
