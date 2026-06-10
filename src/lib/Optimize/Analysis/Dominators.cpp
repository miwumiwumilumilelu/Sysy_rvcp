#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/IR/Instruction.h"
#include <stack>
#include <iostream>

using namespace sysy;

void Dominators::run() {
    buildCFG();

    PostOrder.clear();
    PostOrderNumber.clear();
    std::set<BasicBlock*> visited;
    if (F->getEntryBlock()) {
        computePostOrder(F->getEntryBlock(), visited);
    }

    calculateIDom();

    calculateDomFrontier();
}

void Dominators::buildCFG() {
    Predecessors.clear();
    Successors.clear();

    for (auto bb : F->getBody()->getBlocks()) {
        Predecessors[bb];
        Successors[bb];

        if (bb->getInstructions().empty()) continue;
        
        auto term = bb->getInstructions().back();
        if (auto br = dyn_cast<BranchInst>(term)) {
            for (int i = 0; i < br->getNumOperands(); ++i) {
                if (auto destBB = dyn_cast<BasicBlock>(br->getOperand(i))) {
                    Successors[bb].push_back(destBB);
                    Predecessors[destBB].push_back(bb);
                }
            }
        }
        else if (dyn_cast<ReturnInst>(term)) {
        }
        else {
            // neither branch nor return
            std::cerr << "Warning: BasicBlock " << bb->getName() << " does not end with Br/Ret!\n";
        }
    }
}

void Dominators::computePostOrder(BasicBlock* bb, std::set<BasicBlock*>& visited) {
    visited.insert(bb);
    for (auto succ : Successors[bb]) {
        if (visited.find(succ) == visited.end()) {
            computePostOrder(succ, visited);
        }
    }
    PostOrderNumber[bb] = PostOrder.size();
    PostOrder.push_back(bb);
}

BasicBlock* Dominators::intersect(BasicBlock* b1, BasicBlock* b2) {
    BasicBlock* finger1 = b1;
    BasicBlock* finger2 = b2;
    
    while (finger1 != finger2) {
        while (PostOrderNumber[finger1] < PostOrderNumber[finger2]) {
            finger1 = IDoms[finger1];
        }
        while (PostOrderNumber[finger2] < PostOrderNumber[finger1]) {
            finger2 = IDoms[finger2];
        }
    }
    return finger1;
}

void Dominators::calculateIDom() {
    auto entry = F->getEntryBlock();
    IDoms[entry] = entry;

    bool changed = true;
    while (changed) {
        changed = false;

        for (auto it = PostOrder.rbegin(); it != PostOrder.rend(); ++it) {
            BasicBlock* bb = *it;
            if (bb == entry) continue;

            BasicBlock* new_idom = nullptr;
            for (auto pred : Predecessors[bb]) {
                if (IDoms.count(pred)) {
                    new_idom = pred;
                    break;
                }
            }
            
            if (!new_idom) continue;

            for (auto pred : Predecessors[bb]) {
                if (pred != new_idom && IDoms.count(pred)) {
                    new_idom = intersect(new_idom, pred);
                }
            }

            if (IDoms[bb] != new_idom) {
                IDoms[bb] = new_idom;
                changed = true;
            }
        }
    }

    IDoms[entry] = nullptr;
}

void Dominators::calculateDomFrontier() {
    DomFrontier.clear();
    
    for (auto bb : PostOrder) {
        if (Predecessors[bb].size() >= 2) {
            for (auto pred : Predecessors[bb]) {
                BasicBlock* runner = pred;
                while (runner != IDoms[bb] && runner != nullptr) {
                    DomFrontier[runner].insert(bb);
                    runner = IDoms[runner];
                }
            }
        }
    }
}

bool Dominators::dominates(BasicBlock* A, BasicBlock* B) {
    if (A == B) return true;
    BasicBlock* curr = B;
    while (curr && curr != A) {
        curr = IDoms[curr];
    }
    return curr == A;
}