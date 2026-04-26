#include "Optimize/Scalar/Mem2Reg.h"
#include "IR/Instruction.h"
#include "IR/Type.h"
#include <iostream>
#include <algorithm>
#include <map>

using namespace sysy;

void Mem2Reg::run() {
    int phiCounter = 0;

    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;
        
        Dominators dom(func);
        dom.run();
        this->Dom = &dom; 
        
        promoteMemoryToRegister(func, phiCounter);
        // Unbind before the loop ends to eliminate the risk of dangling pointers.
        this->Dom = nullptr;
    }
}

void Mem2Reg::promoteMemoryToRegister(Function* func, int& phiCounter) {
    PromotableAllocas.clear();
    PhiToAlloca.clear();
    IncomingVals.clear();
    DomTreeChildren.clear();

    buildDomTree(func);
    findPromotableAllocas(func);
    if (PromotableAllocas.empty()) return;

    insertPhiNodes(func, phiCounter);
    rename(func->getEntryBlock());
}

void Mem2Reg::buildDomTree(Function* func) {
    for (auto bb : func->getBody()->getBlocks()) {
        if (auto idom = Dom->getIDom(bb)) {
            if (idom != bb) {
                DomTreeChildren[idom].push_back(bb);
            }
        }
    }
}

bool Mem2Reg::isPromotable(AllocaInst* ai) {
    Type* allocatedTy = cast<PointerType>(ai->getType())->getPointeeType();
    if (allocatedTy->isArray()) return false;

    for (auto user : ai->getUsers()) {
        if (isa<LoadInst>(user)) continue;
        if (auto st = dyn_cast<StoreInst>(user)) {
            if (st->getOperand(1) == ai) continue;
        }
        return false; 
    }
    return true;
}

void Mem2Reg::findPromotableAllocas(Function* func) {
    BasicBlock* entry = func->getEntryBlock();
    for (auto inst : entry->getInstructions()) {
        if (auto ai = dyn_cast<AllocaInst>(inst)) {
            if (isPromotable(ai)) {
                PromotableAllocas.push_back(ai);
            }
        }
    }
}

void Mem2Reg::insertPhiNodes(Function* /*func*/, int& phiCounter) {
    for (auto ai : PromotableAllocas) {
        std::set<BasicBlock*> defBlocks;
        for (auto user : ai->getUsers()) {
            if (auto st = dyn_cast<StoreInst>(user)) {
                if (st->getOperand(1) == ai) { 
                    defBlocks.insert(st->getParent());
                }
            }
        }

        std::set<BasicBlock*> F;
        std::vector<BasicBlock*> W(defBlocks.begin(), defBlocks.end());

        while (!W.empty()) {
            auto X = W.back();
            W.pop_back();

            for (auto Y : Dom->getDomFrontier(X)) {
                if (F.find(Y) == F.end()) {
                    Type* phiTy = cast<PointerType>(ai->getType())->getPointeeType();
                    
                    auto phi = new PhiInst(phiTy, nullptr); 
                    phi->setParent(Y);

                    Y->getInstructions().push_front(phi); 

                    phi->setName("%p" + std::to_string(phiCounter++));

                    PhiToAlloca[phi] = ai;
                    F.insert(Y);
                    
                    if (defBlocks.find(Y) == defBlocks.end()) {
                        defBlocks.insert(Y);
                        W.push_back(Y);
                    }
                }
            }
        }
    }
}

void Mem2Reg::rename(BasicBlock* bb) {
    std::vector<Instruction*> instsToRemove;
    std::map<AllocaInst*, int> pushCount;

    // Lazy init: Reuse the same 0 constant in the same rename() to reduce heap allocation.
    Constant* intZero = nullptr;
    Constant* floatZero = nullptr;
    auto getZeroConstant = [&](AllocaInst* ai) -> Constant* {
        Type* baseType = cast<PointerType>(ai->getType())->getPointeeType();
        if (baseType->isFloat()) {
            if (!floatZero) floatZero = new ConstantFloat(0.0f);
            return floatZero;
        }
        if (!intZero) intZero = new ConstantInt(0);
        return intZero;
    };

    for (auto inst : bb->getInstructions()) {
        if (auto phi = dyn_cast<PhiInst>(inst)) {
            if (PhiToAlloca.count(phi)) {
                AllocaInst* ai = PhiToAlloca[phi];
                IncomingVals[ai].push_back(phi);
                pushCount[ai]++;
            }
        }
        else if (auto load = dyn_cast<LoadInst>(inst)) {
            if (auto ai = dyn_cast<AllocaInst>(load->getOperand(0))) {
                if (std::find(PromotableAllocas.begin(), PromotableAllocas.end(), ai) != PromotableAllocas.end()) {
                    Value* val = nullptr;
                    if (!IncomingVals[ai].empty()) {
                        val = IncomingVals[ai].back();
                    } else {
                        val = getZeroConstant(ai);
                    }
                    load->replaceAllUsesWith(val);
                    instsToRemove.push_back(load);
                }
            }
        }
        else if (auto store = dyn_cast<StoreInst>(inst)) {
            if (auto ai = dyn_cast<AllocaInst>(store->getOperand(1))) {
                if (std::find(PromotableAllocas.begin(), PromotableAllocas.end(), ai) != PromotableAllocas.end()) {
                    Value* val = store->getOperand(0);
                    IncomingVals[ai].push_back(val);
                    pushCount[ai]++;
                    instsToRemove.push_back(store);
                }
            }
        }
        else if (auto allocaInst = dyn_cast<AllocaInst>(inst)) {
            if (std::find(PromotableAllocas.begin(), PromotableAllocas.end(), allocaInst) != PromotableAllocas.end()) {
                instsToRemove.push_back(allocaInst);
            }
        }
    }

    for (auto succ : Dom->getSuccessors(bb)) {
        for (auto inst : succ->getInstructions()) {
            if (auto phi = dyn_cast<PhiInst>(inst)) {
                if (PhiToAlloca.count(phi)) {
                    AllocaInst* ai = PhiToAlloca[phi];
                    if (!IncomingVals[ai].empty()) {
                        phi->addIncoming(IncomingVals[ai].back(), bb);
                    } else {
                        phi->addIncoming(getZeroConstant(ai), bb); 
                    }
                }
            }
        }
    }

    for (auto child : DomTreeChildren[bb]) {
        rename(child);
    }

    for (auto const& [ai, count] : pushCount) {
        for (int k = 0; k < count; ++k) {
            IncomingVals[ai].pop_back();
        }
    }

    while (!instsToRemove.empty()) {
        auto inst = instsToRemove.back();
        instsToRemove.pop_back();

        inst->replaceAllUsesWith(nullptr);
        inst->eraseInst();
    }   
}
