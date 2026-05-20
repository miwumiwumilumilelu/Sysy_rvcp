#include "Optimize/CFG/SimplifyCFG.h"
#include "IR/Instruction.h"
#include <queue>
#include <set>
#include <algorithm>
#include <functional>

using namespace sysy;

bool SimplifyCFG::run() {
    bool anyChanged = false;
    for (auto func : TheModule->getFunctions()) {
        if (func->getBody()->getBlocks().empty()) continue;
        
        bool changed;
        do {
            changed = simplifyFunction(func);
            anyChanged |= changed;
        } while (changed);

        int bbCounter = 0;
        for (auto bb : func->getBody()->getBlocks()) {
            bb->setName("bb" + std::to_string(bbCounter++));
        }
    }
    return anyChanged;
}

// Check if inst is safe to spekulate.
static bool isSafe(Instruction* inst) {
    switch (inst->getOpID()) {
        case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
        case Instruction::Shl: case Instruction::Ashr:
        case Instruction::And: case Instruction::Or:  case Instruction::Xor:
        case Instruction::FAdd: case Instruction::FSub: case Instruction::FMul:
        case Instruction::ICmp: case Instruction::FCmp:
        case Instruction::Select:
            return true;
        default:
            return false;
    }
}

// head:
//   br cond1, check, merge
// check:
//   insts...
//   br cond2, then, merge
// then:
//   result2 = result + power
//   br merge
// merge:
//   result3 = phi [result, head], [result, check], [result2, then]
//
// which represent:
// if (cond1 && cond2)
//     result += power;
//
// Move the pure computations in the check to the head,
// and merge the cond1, cond2
//
// like this:
// head:
//   insts form check...
//   cond = cond1 & cond2
//   br cond, then, merge
//
// This transform allowing it to subsequently switch to SelectInst.
// result += select(cond1 & cond2, 1 << k, 0)
static bool tryHoistConds(Region* region, const std::map<BasicBlock*, std::vector<BasicBlock*>>& preds) {
    auto& blocks = region->getBlocks();
    for (auto* head : blocks) {
        if (head->getInstructions().empty()) continue;

        auto* hbr = dyn_cast<BranchInst>(head->getInstructions().back());
        if (!hbr || hbr->getNumOperands() != 3) continue;
        Value* cond1  = hbr->getOperand(0);
        auto* check = dyn_cast<BasicBlock>(hbr->getOperand(1));
        auto* merge = dyn_cast<BasicBlock>(hbr->getOperand(2));
        // br cond a, a -> br a
        if (!check || !merge || check == merge) continue;

        // Only safely delete this bb if it has only one predecessor.
        auto it = preds.find(check);
        if (it == preds.end() || it->second.size() != 1 || it->second[0] != head) continue;
        if (check->getInstructions().empty()) continue;

        auto* cbr = dyn_cast<BranchInst>(check->getInstructions().back());
        if (!cbr || cbr->getNumOperands() != 3) continue;
        Value* cond2 = cbr->getOperand(0);
        auto* then = dyn_cast<BasicBlock>(cbr->getOperand(1));
        auto* cmerge = dyn_cast<BasicBlock>(cbr->getOperand(2));
        if (!then || cmerge != merge) continue;

        // Because then we need to hoist check insts to head and culculate before CondInst,
        // so Instructions must not have side effects.
        bool allPure = true;
        for (auto* inst : check->getInstructions()) {
            if (dyn_cast<BranchInst>(inst)) continue;
            if (!isSafe(inst)) { 
                allPure = false; 
                break;
            }
        }
        if (!allPure) continue;

        bool phiSafe = true;
        for (auto* inst : merge->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            Value* fromHead = nullptr, *fromCheck = nullptr;
            for (int i = 0; i < phi->getNumOperands(); i += 2) {
                auto* bb = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
                if (bb == head) fromHead = phi->getOperand(i);
                if (bb == check) fromCheck = phi->getOperand(i);
            }
            if (fromHead != fromCheck) { 
                phiSafe = false; 
                break; 
            }
        }
        if (!phiSafe) continue;

// Perform the transformation:
        auto& headInsts = head->getInstructions();
        auto& checkInsts = check->getInstructions();
        auto brPos = std::prev(headInsts.end());

        for (auto it = checkInsts.begin(); it != std::prev(checkInsts.end()); it++) {
            (*it)->setParent(head);
        }
        headInsts.splice(brPos, checkInsts, checkInsts.begin(), std::prev(checkInsts.end()));
        
        // cond = cond1 & cond2
        auto* comb = new BinaryInst(Instruction::And, cond1, cond2, nullptr);
        comb->setParent(head);
        headInsts.insert(brPos, comb);

        // Replace head's branch: br comb, then, merge.
        hbr->setOperand(0, comb);
        hbr->setOperand(1, then);
        hbr->setOperand(2, merge);

        // Update then's phis: check was its predecessor, now head is.
        for (auto* inst : then->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            for (int i = 1; i < phi->getNumOperands(); i += 2) {
                if (dyn_cast<BasicBlock>(phi->getOperand(i)) == check)
                    phi->setOperand(i, head);
            }
        }

        // Remove check's incoming edge from merge's phis.
        for (auto* inst : merge->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            phi->removeIncomingByBlock(check);
        }

        // Delete check (cbr is the only remaining instruction).
        cbr->eraseInst();
        check->replaceAllUsesWith(nullptr);
        blocks.remove(check);
        delete check;

        return true;
    }
    return false;
}

// head:
//   br cond, then, merge
// then:
//   x = ...
//   br merge
// merge:
//   y = phi [old, head], [x, then]
//
// represent:
//
// head:
//   x = ...
//   y = select(cond, x, old)
//   br merge
static bool turnToSelect(Region* region, const std::map<BasicBlock*, std::vector<BasicBlock*>>& preds) {
    auto& blocks = region->getBlocks();
    for (auto* head : blocks) {
        if (head->getInstructions().empty()) continue;
        auto* hbr = dyn_cast<BranchInst>(head->getInstructions().back());
        if (!hbr || hbr->getNumOperands() != 3) continue;

        Value* cond = hbr->getOperand(0);
        auto* then = dyn_cast<BasicBlock>(hbr->getOperand(1));
        auto* merge = dyn_cast<BasicBlock>(hbr->getOperand(2));
        if (!then || !merge || then == merge) continue;

        // then must have predecessor head.
        auto it = preds.find(then);
        if (it == preds.end() || it->second.size() != 1 || it->second[0] != head) continue;

        // merge must have predecessors head and then.
        auto it2 = preds.find(merge);
        if (it2 == preds.end() || it2->second.size() != 2) continue;
        bool hasHead = false, hasThen = false;
        for (auto* p : it2->second) {
            if (p == head) hasHead = true;
            if (p == then) hasThen = true;
        }
        if (!hasHead || !hasThen) continue;

        // then must end with unconditional branch to merge.
        if (then->getInstructions().empty()) continue;
        auto* tbr = dyn_cast<BranchInst>(then->getInstructions().back());
        if (!tbr || tbr->getNumOperands() != 1) continue;
        if (dyn_cast<BasicBlock>(tbr->getOperand(0)) != merge) continue;

        bool allPure = true;
        for (auto* inst : then->getInstructions()) {
            if (dyn_cast<BranchInst>(inst)) continue;
            if (!isSafe(inst)) { 
                allPure = false; 
                break; 
            }
        }
        if (!allPure) continue;
        // Size budget: don't hoist more than 4 instructions.
        int thenSize = (int)then->getInstructions().size() - 1; // exclude br
        if (thenSize > 4) continue;

        // Collect all phis in merge that have entries from head and then.
        // All must be handled together.
        std::vector<PhiInst*> phis;
        for (auto* inst : merge->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            phis.push_back(phi);
        }

        // Skip if any phi has float type.
        bool hasFloat = false;
        for (auto* phi : phis) {
            if (phi->getType() && phi->getType()->isFloat()) { hasFloat = true; break; }
        }
        if (hasFloat) continue;

// Perform the transformation
        auto& headInsts = head->getInstructions();
        auto& thenInsts = then->getInstructions();
        auto brPos = std::find(headInsts.begin(), headInsts.end(), static_cast<Instruction*>(hbr));

        // Hoist then's pure instructions into head (before the branch).
        for (auto it = thenInsts.begin(); it != std::prev(thenInsts.end()); it++) {
            (*it)->setParent(head);
        }

        headInsts.splice(brPos, thenInsts, thenInsts.begin(), std::prev(thenInsts.end()));

        // For each phi, create select(cond, v_then, v_head) and replace the phi.
        for (auto* phi : phis) {
            Value* v_head = nullptr, *v_then = nullptr;
            for (int i = 0; i < phi->getNumOperands(); i += 2) {
                auto* bb = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
                if (bb == head) v_head = phi->getOperand(i);
                if (bb == then) v_then = phi->getOperand(i);
            }
            if (!v_head || !v_then) continue;

            // Insert select before head's branch.
            auto* sel = new SelectInst(cond, v_then, v_head, nullptr);
            sel->setName(phi->getName());
            sel->setParent(head);
            headInsts.insert(brPos, sel);

            phi->replaceAllUsesWith(sel);
            phi->eraseInst();
        }

        hbr->replaceAllUsesWith(nullptr);
        hbr->eraseInst();
        new BranchInst(merge, head);

        tbr->eraseInst();
        then->replaceAllUsesWith(nullptr);
        blocks.remove(then);
        delete then;

        return true;
    }
    return false;
}

bool SimplifyCFG::simplifyFunction(Function* func) {
    bool changed = false;
    Region* region = func->getBody();

    changed |= removeGhostPhiEdges(region);
    changed |= simplifyBranches(region);

    changed |= eliminateUnreachableBlocks(region);
    changed |= mergeBasicBlocks(region);
    changed |= eliminateEmptyBlocks(region);

    auto preds = computePredecessors(region);
    if (tryHoistConds(region, preds)) return true;
    preds = computePredecessors(region);
    if (turnToSelect(region, preds)) return true;

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

    // Use std::set to avoid duplicate.
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
                deadInst->replaceAllUsesWith(nullptr);
                deadInst->eraseInst();
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
                        ++succIt;
                        phi->eraseInst();
                    } else {
                        break;
                    }
                }
                
                term->replaceAllUsesWith(nullptr);
                term->eraseInst();

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
                    br->eraseInst();

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
                std::vector<int> invalid;
                for (int i = 0; i < phi->getNumOperands(); i += 2) {
                    BasicBlock* inBB = dyn_cast<BasicBlock>(phi->getOperand(i + 1));
                    if (!phi->getOperand(i) || !inBB) {
                        invalid.push_back(i);
                    } else if (std::find(bbPreds.begin(), bbPreds.end(), inBB) == bbPreds.end()) {
                        ghosts.push_back(inBB);
                    }
                }
                // remove [null, null] pair in PHI.
                for (auto it = invalid.rbegin(); it != invalid.rend(); ++it) {
                    phi->removeIncomingAt(*it);
                    changed = true;
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
                        br->replaceAllUsesWith(nullptr);
                        br->eraseInst();

                        new BranchInst(dest, bb);
                        changed = true;
                    }
                }
            }
        }
    }
    return changed;
}
