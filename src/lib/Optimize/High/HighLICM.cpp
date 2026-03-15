#include "Optimize/High/HighLICM.h"
#include <algorithm>

using namespace sysy;

// Strip GEP chain to find the base pointer of a memory access.
static Value* getBase(Value* ptr) {
    while (auto gep = dyn_cast<GetElementPtrInst>(ptr))
        ptr = gep->getOperand(0);
    return ptr;
}

void HighLICM::collectDirectMods(Region* region, std::set<GlobalVariable*>& mods) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto st = dyn_cast<StoreInst>(inst)) {
                if (auto gv = dyn_cast<GlobalVariable>(getBase(st->getOperand(1))))
                    mods.insert(gv);
            } else if (auto wi = dyn_cast<WhileInst>(inst)) {
                collectDirectMods(wi->getCondRegion(), mods);
                collectDirectMods(wi->getBodyRegion(), mods);
            } else if (auto ii = dyn_cast<IfInst>(inst)) {
                collectDirectMods(ii->getThenRegion(), mods);
                if (ii->getElseRegion())
                    collectDirectMods(ii->getElseRegion(), mods);
            }
        }
    }
}

void HighLICM::collectCallees(Region* region, std::set<Function*>& callees) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto call = dyn_cast<CallInst>(inst)) {
                if (auto callee = call->getFunction())
                    callees.insert(callee);
            } else if (auto wi = dyn_cast<WhileInst>(inst)) {
                collectCallees(wi->getCondRegion(), callees);
                collectCallees(wi->getBodyRegion(), callees);
            } else if (auto ii = dyn_cast<IfInst>(inst)) {
                collectCallees(ii->getThenRegion(), callees);
                if (ii->getElseRegion())
                    collectCallees(ii->getElseRegion(), callees);
            }
        }
    }
}

void HighLICM::computeModSets() {
    // Direct global writes per function (skip external functions).
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;
        collectDirectMods(func->getBody(), ModSets[func]);
    }

    // Worklist propagation: modSet(caller) |= modSet(callee).
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto func : TheModule->getFunctions()) {
            if (func->getBody()->getBlocks().empty()) continue;
            std::set<Function*> callees;
            collectCallees(func->getBody(), callees);
            for (auto callee : callees) {
                for (auto gv : ModSets[callee]) {
                    if (ModSets[func].insert(gv).second)
                        changed = true;
                }
            }
        }
    }
}

void HighLICM::run() {
    computeModSets();
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

static bool noAliasWithAllStores(Value* load_ptr,
                            Value* load_base,
                            const std::set<Value*>& storeGepPtrs) {
    auto* load_gep = dyn_cast<GetElementPtrInst>(load_ptr);
    if (!load_gep) return false;
    auto* load_ci = dyn_cast<ConstantInt>(load_gep->getOperand(1));
    if (!load_ci) return false;

    for (auto* sptr : storeGepPtrs) {
        if (getBase(sptr) != load_base) continue; 
        auto* store_gep = dyn_cast<GetElementPtrInst>(sptr);
        if (!store_gep) return false;
        auto* store_ci = dyn_cast<ConstantInt>(store_gep->getOperand(1));
        if (!store_ci) return false;  
        if (store_ci->getValue() == load_ci->getValue()) return false; 
    }
    return true; 
}

void HighLICM::scanLoop(Region* region, std::set<Value*>& storeBases,
                    std::set<Value*>& storeGepPtrs,
                    std::set<GlobalVariable*>& callModGlobals, bool& loopHasCall) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto st = dyn_cast<StoreInst>(inst)) {
                Value* sptr = st->getOperand(1);
                storeBases.insert(getBase(sptr));
                storeGepPtrs.insert(sptr);
            } else if (auto call = dyn_cast<CallInst>(inst)) {
                loopHasCall = true;
                Function* callee = call->getFunction();
                if (callee && !callee->getBody()->getBlocks().empty()) {
                    for (auto gv : ModSets[callee])
                        callModGlobals.insert(gv);
                } else {
                    // External/unknown function: conservatively poison all globals.
                    for (auto gv : TheModule->getGlobals())
                        callModGlobals.insert(gv);
                }
            } else if (auto wi = dyn_cast<WhileInst>(inst)) {
                scanLoop(wi->getCondRegion(), storeBases, storeGepPtrs, callModGlobals, loopHasCall);
                scanLoop(wi->getBodyRegion(), storeBases, storeGepPtrs, callModGlobals, loopHasCall);
            } else if (auto ii = dyn_cast<IfInst>(inst)) {
                scanLoop(ii->getThenRegion(), storeBases, storeGepPtrs, callModGlobals, loopHasCall);
                if (ii->getElseRegion())
                    scanLoop(ii->getElseRegion(), storeBases, storeGepPtrs, callModGlobals, loopHasCall);
            }
        }
    }
}

bool HighLICM::isHoistable(Instruction* inst,
                        const std::set<Value*>& loopDefs,
                        const std::set<Value*>& storeBases,
                        const std::set<Value*>& storeGepPtrs,
                        const std::set<GlobalVariable*>& callModGlobals,
                        bool loopHasCall) {
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
            Value* load_ptr = inst->getOperand(0);
            Value* base = getBase(load_ptr);
            // If any loop store targets the same base, check for NoAlias before blocking.
            if (storeBases.count(base)) {
                if (!noAliasWithAllStores(load_ptr, base, storeGepPtrs))
                    return false;
            }
            if (auto gv = dyn_cast<GlobalVariable>(base)) {
                if (callModGlobals.count(gv)) return false;
            } else {
                if (loopHasCall) return false;
            }
            break;
        }
        default:
            return false;
    }
    // All operands must be loop-invariant (defined outside the loop).
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
    std::set<Value*> storeGepPtrs;
    std::set<GlobalVariable*> callModGlobals;
    bool loopHasCall = false;
    scanLoop(wi->getBodyRegion(), storeBases, storeGepPtrs, callModGlobals, loopHasCall);
    scanLoop(wi->getCondRegion(), storeBases, storeGepPtrs, callModGlobals, loopHasCall);

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
                    if (isHoistable(inst, loopDefs, storeBases, storeGepPtrs, callModGlobals, loopHasCall)) {
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
