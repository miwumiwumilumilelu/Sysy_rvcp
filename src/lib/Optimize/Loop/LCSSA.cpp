#include "Optimize/Loop/LCSSA.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <map>
#include <vector>

using namespace sysy;

Value* LCSSA::findValue(BasicBlock* bb,
                        std::map<BasicBlock*, Value*>& reachMap,
                        Dominators& dt) {
    auto it = reachMap.find(bb);
    if (it != reachMap.end()) return it->second;

    auto* idom = dt.getIDom(bb);
    // TODO: handle multi-exit loops by bridging phis along the idom chain (reference project's getValueFor).
    if (!idom || idom == bb) return reachMap[bb] = nullptr;

    return reachMap[bb] = findValue(idom, reachMap, dt);
}

static void insertAtPhiEnd(BasicBlock* bb, PhiInst* phi) {
    auto& insts = bb->getInstructions();
    auto it = insts.begin();
    while (it != insts.end() && dyn_cast<PhiInst>(*it))
        ++it;
    insts.insert(it, phi);
}

bool LCSSA::processLoop(Loop* L, Dominators& dt) {
    bool changed = false;
    for (auto sub : L->sub)
        changed |= processLoop(sub, dt);

    if (!L->hasPreheaderAndSingleLatch()) return changed; 
    if (!hasDedicatedExits(L, dt)) return changed;
    if (L->exits.size() != 1) return changed;

    BasicBlock* exit = L->exits[0];
    // Snapshot
    std::vector<BasicBlock*> loopBlocks(L->blocks.begin(), L->blocks.end());

    for (auto* bb : loopBlocks) {
        // Snapshot as iterater while modifying instructions.
        std::vector<Instruction*> insts;
        for (auto* inst : bb->getInstructions())
            insts.push_back(inst);

        for (auto* def : insts) {
            if (def->getType()->isVoid()) continue;

            // Collect uses outside the loop.
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
                        // PhiInst operands: [val0, bb0, def, bb1, ...]
                        // bb1 is the effective use block for def.
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

            // Only insert if def's block dominates the exit, 
            // so every loop predecessor incoming of the LCSSA phi is defined.
            if (!dt.dominates(bb, exit)) continue;

            // Build LCSSA phi at the end of exit's phi section.
            auto* phi = new PhiInst(def->getType(), nullptr);
            phi->setName(def->getName() + ".lc");
            phi->setParent(exit);
            for (auto* pred : dt.getPredecessors(exit)) {
                if (L->has(pred))
                    phi->addIncoming(def, pred);
            }
            insertAtPhiEnd(exit, phi);

            // Rewrite each outside use.
            //
            // use -> def [in loop]
            // becomes:
            // use -> def.lc [in exit]
            //
            std::map<BasicBlock*, Value*> reachMap;
            reachMap[exit] = phi;

            for (auto& use : outsideUses) {
                Value* repl = findValue(use.effectiveBB, reachMap, dt);
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
