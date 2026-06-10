#include "../../../include/Optimize/Loop/LCSSA.h"
#include "../../../include/IR/Instruction.h"
#include <algorithm>
#include <map>
#include <vector>

using namespace sysy;

static void insertAtPhiEnd(BasicBlock* bb, PhiInst* phi) {
    auto& insts = bb->getInstructions();
    auto it = insts.begin();
    while (it != insts.end() && dyn_cast<PhiInst>(*it))
        ++it;
    insts.insert(it, phi);
}

// Walk up from bb to find the nearest LCSSA phi seeded in reachMap.
Value* LCSSA::findValue(BasicBlock* bb,
                        std::map<BasicBlock*, Value*>& reachMap,
                        Dominators& dt,
                        const std::set<BasicBlock*>& loopBBs) {
    auto it = reachMap.find(bb);
    if (it != reachMap.end()) return it->second;

    // Sentinel: break cycles in the loop-external CFG.
    reachMap[bb] = nullptr;

    // Fast path: idom chain.
    auto* idom = dt.getIDom(bb);
    if (idom && idom != bb) {
        if (Value* v = findValue(idom, reachMap, dt, loopBBs))
            return reachMap[bb] = v;
    }

    // Bridging phi: only for loop-external join blocks.
    // Fires only when every predecessor carries a non-null value and
    // at least two of them differ (otherwise no merge is needed).
    if (loopBBs.count(bb)) return nullptr;

    const auto& preds = dt.getPredecessors(bb);
    if (preds.size() < 2) return nullptr;

    std::vector<Value*> vals;
    vals.reserve(preds.size());
    for (auto* pred : preds) {
        Value* v = findValue(pred, reachMap, dt, loopBBs);
        if (!v) return nullptr;     // Incomplete: bail out conservatively.
        vals.push_back(v);
    }

    Value* ref = vals[0];
    bool allSame = true;
    for (auto* v : vals)
        if (v != ref) { allSame = false; break; }
    if (allSame) return reachMap[bb] = ref;

    // All non-null, not all same, and outside the loop → bridging phi.
    auto* phi = new PhiInst(cast<Instruction>(ref)->getType(), nullptr);
    phi->setParent(bb);
    reachMap[bb] = phi;     // Cache before incomings in case of back-reference.
    for (size_t i = 0; i < preds.size(); i++)
        phi->addIncoming(vals[i], preds[i]);
    insertAtPhiEnd(bb, phi);
    return phi;
}

bool LCSSA::processLoop(Loop* L, Dominators& dt) {
    bool changed = false;
    for (auto sub : L->sub)
        changed |= processLoop(sub, dt);

    if (!L->hasPreheaderAndSingleLatch()) return changed;
    if (!hasDedicatedExits(L, dt)) return changed;

    std::set<BasicBlock*> loopBBsSet(L->blocks.begin(), L->blocks.end());
    std::vector<BasicBlock*> loopBlocks(L->blocks.begin(), L->blocks.end());

    for (auto* bb : loopBlocks) {
        std::vector<Instruction*> insts;
        for (auto* inst : bb->getInstructions())
            insts.push_back(inst);

        for (auto* def : insts) {
            if (def->getType()->isVoid()) continue;

            struct OutsideUse {
                int idx;
                User* user;
                BasicBlock* effectiveBB;
            };

            std::vector<OutsideUse> outsideUses;

            std::vector<User*> users = def->getUsers();
            for (auto* user : users) {
                auto* userInst = dyn_cast<Instruction>(user);
                if (!userInst) continue;

                for (int i = 0; i < user->getNumOperands(); ++i) {
                    if (user->getOperand(i) != def) continue;

                    BasicBlock* effectiveBB;
                    if (auto* phi = dyn_cast<PhiInst>(userInst)) {
                        if (i % 2 != 0) continue;
                        effectiveBB = cast<BasicBlock>(phi->getOperand(i + 1));
                    } else {
                        effectiveBB = userInst->getParent();
                    }

                    if (!L->has(effectiveBB))
                        outsideUses.push_back({i, user, effectiveBB});
                }
            }

            if (outsideUses.empty()) continue;

            // Build one LCSSA phi per exit dominated by def's block.
            std::map<BasicBlock*, Value*> reachMap;
            for (auto* exitBB : L->exits) {
                if (!dt.dominates(bb, exitBB)) continue;
                auto* phi = new PhiInst(def->getType(), nullptr);
                phi->setName(def->getName() + ".lc." + exitBB->getName());
                phi->setParent(exitBB);
                for (auto* pred : dt.getPredecessors(exitBB)) {
                    if (L->has(pred))
                        phi->addIncoming(def, pred);
                }
                insertAtPhiEnd(exitBB, phi);
                reachMap[exitBB] = phi;
            }

            if (reachMap.empty()) continue;

            for (auto& use : outsideUses) {
                Value* repl = findValue(use.effectiveBB, reachMap, dt, loopBBsSet);
                if (repl)
                    use.user->setOperand(use.idx, repl);
            }

            changed = true;
        }
    }

    return changed;
}

bool LCSSA::runOnFunction(Function* f) {
    if (!f || f->getBody()->getBlocks().empty()) return false;

    Dominators dt(f);
    dt.run();
    LoopInfo li(f, dt);

    bool changed = false;
    for (auto* top : li.tops())
        changed |= processLoop(top, dt);

    return changed;
}

bool LCSSA::run() {
    bool changed = false;
    for (auto* f : M->getFunctions())
        changed |= runOnFunction(f);
    return changed;
}
