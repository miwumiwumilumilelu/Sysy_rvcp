#include "Optimize/SimplifyCFG.h"
#include "IR/Instruction.h"
#include <queue>
#include <set>
#include <algorithm>

using namespace sysy;

void SimplifyCFG::run() {
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;
        
        bool changed;
        do {
            changed = simplifyFunction(func);
        } while (changed);

        int bbCounter = 0;
        for (auto bb : func->getBody()->getBlocks()) {
            bb->setName("bb" + std::to_string(bbCounter++));
        }
    }
}

bool SimplifyCFG::simplifyFunction(Function* func) {
    bool changed = false;
    Region* region = func->getBody();

    changed |= removeGhostPhiEdges(region); 
    changed |= simplifyBranches(region);

    changed |= eliminateUnreachableBlocks(region);
    changed |= mergeBasicBlocks(region);
    changed |= eliminateEmptyBlocks(region);
    
    return changed;
}

std::map<BasicBlock*, std::vector<BasicBlock*>> SimplifyCFG::computePredecessors(Region* region) {
    std::map<BasicBlock*, std::vector<BasicBlock*>> preds;
    for (auto bb : region->getBlocks()) preds[bb] = {};
    for (auto bb : region->getBlocks()) {
        if (bb->getInstructions().empty()) continue;
        auto term = bb->getInstructions().back();
        if (auto br = dyn_cast<BranchInst>(term)) {
            if (br->getNumOperands() == 3) {
                if (auto t = dyn_cast<BasicBlock>(br->getOperand(1))) preds[t].push_back(bb);
                if (auto f = dyn_cast<BasicBlock>(br->getOperand(2))) preds[f].push_back(bb);
            } else if (br->getNumOperands() == 1) {
                if (auto d = dyn_cast<BasicBlock>(br->getOperand(0))) preds[d].push_back(bb);
            }
        }
    }

    for (auto& pair : preds) {
        auto& vec = pair.second;
        std::set<BasicBlock*> s(vec.begin(), vec.end());
        vec.assign(s.begin(), s.end());
    }
    return preds;
}

bool SimplifyCFG::eliminateUnreachableBlocks(Region* region) {
    bool changed = false;
    std::set<BasicBlock*> reachable;
    std::queue<BasicBlock*> q;
    
    auto entry = region->getEntryBlock();
    q.push(entry);
    reachable.insert(entry);

    while (!q.empty()) {
        auto curr = q.front(); q.pop();
        if (curr->getInstructions().empty()) continue;
        auto term = curr->getInstructions().back();
        if (auto br = dyn_cast<BranchInst>(term)) {
            if (br->getNumOperands() == 3) {
                if (auto t = dyn_cast<BasicBlock>(br->getOperand(1))) {
                    if (reachable.insert(t).second) q.push(t);
                }
                if (auto f = dyn_cast<BasicBlock>(br->getOperand(2))) {
                    if (reachable.insert(f).second) q.push(f);
                }
            } else if (br->getNumOperands() == 1) {
                if (auto dest = dyn_cast<BasicBlock>(br->getOperand(0))) {
                    if (reachable.insert(dest).second) q.push(dest);
                }
            }
        }
    }

    auto& blocks = region->getBlocks();
    for (auto it = blocks.begin(); it != blocks.end(); ) {
        BasicBlock* bb = *it;
        if (reachable.find(bb) == reachable.end()) {
            if (!bb->getInstructions().empty()) {
                auto term = bb->getInstructions().back();
                if (auto br = dyn_cast<BranchInst>(term)) {
                    if (br->getNumOperands() == 3) {
                        if (auto t = dyn_cast<BasicBlock>(br->getOperand(1))) {
                            for (auto inst : t->getInstructions())
                                if (auto phi = dyn_cast<PhiInst>(inst)) phi->removeIncomingByBlock(bb); else break;
                        }
                        if (auto f = dyn_cast<BasicBlock>(br->getOperand(2))) {
                            for (auto inst : f->getInstructions())
                                if (auto phi = dyn_cast<PhiInst>(inst)) phi->removeIncomingByBlock(bb); else break;
                        }
                    } else if (br->getNumOperands() == 1) {
                        if (auto d = dyn_cast<BasicBlock>(br->getOperand(0))) {
                            for (auto inst : d->getInstructions())
                                if (auto phi = dyn_cast<PhiInst>(inst)) phi->removeIncomingByBlock(bb); else break;
                        }
                    }
                }
            }

            auto& insts = bb->getInstructions();
            while (!insts.empty()) {
                auto deadInst = insts.back();
                insts.pop_back();
                deadInst->replaceAllUsesWith(nullptr);
                for(int i = 0; i < deadInst->getNumOperands(); i++) deadInst->setOperand(i, nullptr);
                deadInst->setParent(nullptr);
                delete deadInst;
            }

            bb->replaceAllUsesWith(nullptr);
            it = blocks.erase(it);

            delete bb;
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

bool SimplifyCFG::mergeBasicBlocks(Region* region) {
    auto preds = computePredecessors(region);
    auto& blocks = region->getBlocks();
    
    for (auto bb : blocks) {
        if (bb->getInstructions().empty()) continue;
        auto term = dyn_cast<BranchInst>(bb->getInstructions().back());
        
        if (term && term->getNumOperands() == 1) {
            BasicBlock* succ = dyn_cast<BasicBlock>(term->getOperand(0));

            if (succ != bb && preds[succ].size() == 1 && succ != region->getEntryBlock()) {
                auto& succInsts = succ->getInstructions();
                for (auto succIt = succInsts.begin(); succIt != succInsts.end(); ) {
                    if (auto phi = dyn_cast<PhiInst>(*succIt)) {
                        Value* incomingVal = nullptr;
                        for (int i = 0; i < phi->getNumOperands(); i += 2) {
                            if (phi->getOperand(i+1) == bb) {
                                incomingVal = phi->getOperand(i); break;
                            }
                        }
                        if (incomingVal) phi->replaceAllUsesWith(incomingVal);
                        for(int i = 0; i < phi->getNumOperands(); i++) phi->setOperand(i, nullptr);
                        phi->setParent(nullptr);
                        succIt = succInsts.erase(succIt);
                        delete phi;
                    } else {
                        break;
                    }
                }
                
                bb->getInstructions().remove(term);

                term->replaceAllUsesWith(nullptr);
                for(int i = 0; i < term->getNumOperands(); i++) term->setOperand(i, nullptr);
                term->setParent(nullptr);
                delete term;

                bb->getInstructions().splice(bb->getInstructions().end(), succInsts);

                for (auto inst : bb->getInstructions()) {
                    inst->setParent(bb);
                }

                succ->replaceAllUsesWith(bb);
                
                auto succItInList = std::find(blocks.begin(), blocks.end(), succ);
                if (succItInList != blocks.end()) {
                    blocks.erase(succItInList);
                    delete succ;
                } 
                return true; 
            }
        }
    }
    return false;
}

bool SimplifyCFG::eliminateEmptyBlocks(Region* region) {
    auto preds = computePredecessors(region);
    auto& blocks = region->getBlocks();
    
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
        BasicBlock* bb = *it;
        if (bb == region->getEntryBlock()) continue;
        
        auto& insts = bb->getInstructions();
        if (insts.size() == 1) { 
            if (auto br = dyn_cast<BranchInst>(insts.front())) {
                if (br->getNumOperands() == 1) {
                    BasicBlock* succ = dyn_cast<BasicBlock>(br->getOperand(0));
                    if (succ == bb) continue;
                    
                    auto& bbPreds = preds[bb];
                    if (bbPreds.empty()) continue;

                    bool hasPhi = false;
                    for (auto inst : succ->getInstructions()) {
                        if (isa<PhiInst>(inst)) { hasPhi = true; break; }
                    }
                    if (hasPhi) {
                        bool safe = true;
                        auto& succPreds = preds[succ];
                        for (auto pred : bbPreds) {
                            if (std::find(succPreds.begin(), succPreds.end(), pred) != succPreds.end() ||
                                std::count(bbPreds.begin(), bbPreds.end(), pred) > 1) {
                                safe = false; break;
                            }
                        }
                        if (!safe) continue;
                    }

                    for (auto inst : succ->getInstructions()) {
                        if (auto phi = dyn_cast<PhiInst>(inst)) {
                            Value* valFromBB = nullptr;
                            for (int i = 0; i < phi->getNumOperands(); i += 2) {
                                if (phi->getOperand(i+1) == bb) {
                                    valFromBB = phi->getOperand(i); break;
                                }
                            }
                            if (valFromBB) {
                                phi->removeIncomingByBlock(bb);
                                for (auto pred : bbPreds) phi->addIncoming(valFromBB, pred); // 使用您手写的 addIncoming!
                            }
                        } else { break; }
                    }

                    for (auto pred : bbPreds) {
                        auto predTerm = pred->getInstructions().back();
                        if (auto predBr = dyn_cast<BranchInst>(predTerm)) {
                            predBr->replaceSuccessor(bb, succ);
                        }
                    }
                    bb->replaceAllUsesWith(nullptr);
                    br->replaceAllUsesWith(nullptr);
                    for(int i = 0; i < br->getNumOperands(); i++) br->setOperand(i, nullptr);
                    br->setParent(nullptr);
                    delete br;

                    blocks.erase(it);
                    delete bb; 
                    return true;
                }
            }
        }
    }
    return false;
}

bool SimplifyCFG::removeGhostPhiEdges(Region* region) {
    bool changed = false;
    auto preds = computePredecessors(region);
    for (auto bb : region->getBlocks()) {
        auto& bbPreds = preds[bb];
        for (auto inst : bb->getInstructions()) {
            if (auto phi = dyn_cast<PhiInst>(inst)) {
            // Only store BasicBlock* that have been confirmed by dyn_cast, eliminate UB from C-style casting.
                std::vector<BasicBlock*> ghosts;
                for (int i = 0; i < phi->getNumOperands(); i += 2) {
                    BasicBlock* inBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
                    if (inBB && std::find(bbPreds.begin(), bbPreds.end(), inBB) == bbPreds.end()) {
                        ghosts.push_back(inBB);
                    }
                }
                for (auto g : ghosts) {
                    phi->removeIncomingByBlock(g);
                    changed = true;
                }
            } else {
                break;
            }
        }
    }
    return changed;
}

// br cond bb1, bb1
// 
// become:
//
// br bb1
bool SimplifyCFG::simplifyBranches(Region* region) {
    bool changed = false;
    for (auto bb : region->getBlocks()) {
        if (bb->getInstructions().empty()) continue;
        auto term = bb->getInstructions().back();
        if (auto br = dyn_cast<BranchInst>(term)) {
            if (br->getNumOperands() == 3) {
                if (br->getOperand(1) == br->getOperand(2)) {
                    if (BasicBlock* dest = dyn_cast<BasicBlock>(br->getOperand(1))) {
                        bb->getInstructions().remove(br);

                        br->replaceAllUsesWith(nullptr);
                        for(int i = 0; i < br->getNumOperands(); i++) br->setOperand(i, nullptr);
                        br->setParent(nullptr);
                        delete br;

                        new BranchInst(dest, bb);
                        changed = true;
                    }
                }
            }
        }
    }
    return changed;
}

