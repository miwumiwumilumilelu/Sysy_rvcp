#include "Optimize/Loop/LICM.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>

using namespace sysy;

// Follow GEP/phi chains to the root pointer (global, alloca, or arg).
static Value* getBaseObject(Value* v, std::set<Value*>& vis) {
    if (!vis.insert(v).second) return v;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return getBaseObject(gep->getOperand(0), vis);
    if (auto* phi = dyn_cast<PhiInst>(v)) {
        Value* base = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            Value* b = getBaseObject(phi->getOperand(k), vis);
            if (!base) base = b;
            else if (base != b) return v;
        }
        return base ? base : v;
    }
    return v;
}
static Value* getBaseObject(Value* v) {
    std::set<Value*> vis;
    return getBaseObject(v, vis);
}

// True iff f contains no stores (transitively). Conservative for external fns.
static bool isReadOnlyFunc(Function* f, std::unordered_map<Function*, bool>& cache) {
    if (!f) return false;
    auto* body = f->getBody();
    if (!body || body->getBlocks().empty()) return false;
    auto it = cache.find(f);
    if (it != cache.end()) return it->second;
    cache[f] = true;
    for (auto bb : body->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (isa<StoreInst>(inst)) { cache[f] = false; return false; }
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isReadOnlyFunc(call->getFunction(), cache)) {
                    cache[f] = false; return false;
                }
        }
    }
    return true;
}

// Clone inst into target before its terminator, remapping operands via vmap.
static Instruction* cloneInst(Instruction* inst, BasicBlock* tgt,
                               const std::unordered_map<Value*, Value*>& vmap) {
    auto remap = [&](Value* v) -> Value* {
        auto it = vmap.find(v);
        return it != vmap.end() ? it->second : v;
    };
    auto op = inst->getOpID();
    if (isa<BinaryInst>(inst))
        return new BinaryInst(op, remap(inst->getOperand(0)), remap(inst->getOperand(1)), tgt);
    if (auto* ic = dyn_cast<ICmpInst>(inst))
        return new ICmpInst(ic->getPredicate(), remap(inst->getOperand(0)), remap(inst->getOperand(1)), tgt);
    if (auto* fc = dyn_cast<FCmpInst>(inst))
        return new FCmpInst(fc->getPredicate(), remap(inst->getOperand(0)), remap(inst->getOperand(1)), tgt);
    if (isa<CastInst>(inst))
        return new CastInst(op, remap(inst->getOperand(0)), inst->getType(), tgt);
    if (isa<GetElementPtrInst>(inst))
        return new GetElementPtrInst(remap(inst->getOperand(0)), remap(inst->getOperand(1)), tgt);
    return nullptr;
}

bool LICM::run() {
    bool any = false;
    purityCache.clear();
    readOnlyCache.clear();
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool LICM::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    bool changed = false;

    // rotate while(cond){body} → if(cond){do{body}while(cond)}.
    {
        bool rot;
        do {
            rot = false;
            Dominators dt(f); dt.run();
            LoopInfo li(f, dt);
            std::function<bool(Loop*)> visit = [&](Loop* L) -> bool {
                for (auto sub : L->sub) if (visit(sub)) return true;
                return rotateLoop(L, f);
            };
            for (auto top : li.tops())
                if (visit(top)) { changed = rot = true; break; }
        } while (rot);
    }

    // instruction-level LICM, inner-first.
    {
        Dominators dt(f); dt.run();
        LoopInfo li(f, dt);
        SCEV scev(f, li);
        std::function<void(Loop*)> visit = [&](Loop* L) {
            for (auto sub : L->sub) visit(sub);
            changed |= hoistLoop(L, dt, scev);
        };
        for (auto top : li.tops()) visit(top);
    }

    // hoist entire outer-invariant inner loops before the outer loop.
    bool hoisted;
    do {
        hoisted = false;
        Dominators dt(f); dt.run();
        LoopInfo li(f, dt);
        for (auto outer : li.tops()) {
            if (tryHoistSubloop(outer)) {
                changed = hoisted = true;
                break;
            }
        }
    } while (hoisted);

    return changed;
}

// Rotate loop: pre→head(cond→body/exit), latch→head(uncond)
//          -> pre(cond->head/exit), head->body(uncond), latch(cond->head/exit)
bool LICM::rotateLoop(Loop* L, Function* f) {
    if (!L->head || !L->latch || !L->pre) return false;

    // Require unique back-edge from latch (single unconditional branch to head).
    {
        int cnt = 0;
        for (auto bb : L->blocks) {
            auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
            if (!br) continue;
            for (int k = 0; k < (int)br->getNumOperands(); k++)
                if (auto* s = dyn_cast<BasicBlock>(br->getOperand(k)))
                    if (s == L->head) { cnt++; break; }
        }
        if (cnt != 1) return false;
    }
    auto* lbr = dyn_cast<BranchInst>(L->latch->getInstructions().back());
    if (!lbr || lbr->getNumOperands() != 1) return false;
    if (cast<BasicBlock>(lbr->getOperand(0)) != L->head) return false;

    // Head must end with conditional branch: one successor in loop (body), one out (exit).
    auto* hbr = dyn_cast<BranchInst>(L->head->getInstructions().back());
    if (!hbr || hbr->getNumOperands() != 3) return false;
    BasicBlock* t1 = cast<BasicBlock>(hbr->getOperand(1));
    BasicBlock* t2 = cast<BasicBlock>(hbr->getOperand(2));
    std::set<BasicBlock*> loopBBs(L->blocks.begin(), L->blocks.end());
    if (loopBBs.count(t1) == loopBBs.count(t2)) return false;
    BasicBlock* body = loopBBs.count(t1) ? t1 : t2;
    BasicBlock* exit = loopBBs.count(t1) ? t2 : t1;

    // Pre must be unconditional branch to head.
    auto* pbr = dyn_cast<BranchInst>(L->pre->getInstructions().back());
    if (!pbr || pbr->getNumOperands() != 1) return false;
    if (cast<BasicBlock>(pbr->getOperand(0)) != L->head) return false;

    // Single-exit: no loop block other than head may branch to exit.
    for (auto bb : loopBBs) {
        if (bb == L->head) continue;
        auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
        if (!br) continue;
        for (int k = 0; k < (int)br->getNumOperands(); k++)
            if (auto* s = dyn_cast<BasicBlock>(br->getOperand(k)))
                if (s == exit) return false;
    }

    Value* cond = hbr->getOperand(0);
    if (!cond) return false; // while(1): condition folded, skip

    // Collect head phis and condition-computation chain.
    std::set<Instruction*> headPhis;
    for (auto inst : L->head->getInstructions()) {
        if (!isa<PhiInst>(inst)) break;
        headPhis.insert(inst);
    }

    std::set<Instruction*> chain;
    {
        std::vector<Value*> wl = {cond};
        while (!wl.empty()) {
            Value* v = wl.back(); wl.pop_back();
            auto* def = dyn_cast<Instruction>(v);
            if (!def || def->getParent() != L->head) continue;
            if (headPhis.count(def) || chain.count(def)) continue;
            chain.insert(def);
            for (int k = 0; k < (int)def->getNumOperands(); k++)
                wl.push_back(def->getOperand(k));
        }
    }

    // Refuse if chain contains memory ops (unsafe to speculate).
    for (auto* inst : chain)
        if (isa<LoadInst>(inst) || isa<StoreInst>(inst) || isa<CallInst>(inst))
            return false;

    // Head must contain only phis + chain + branch.
    for (auto inst : L->head->getInstructions()) {
        if (isa<PhiInst>(inst) || isa<BranchInst>(inst) || chain.count(inst)) continue;
        return false;
    }

    // phi -> {init value from pre, iter value from latch}
    std::unordered_map<Value*, Value*> initMap, latMap;
    for (auto* pi : headPhis) {
        auto* phi = cast<PhiInst>(pi);
        Value* iv = nullptr, *lv = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* src = cast<BasicBlock>(phi->getOperand(k + 1));
            if (src == L->pre)   iv = phi->getOperand(k);
            else if (src == L->latch) lv = phi->getOperand(k);
        }
        if (!iv || !lv) return false;
        initMap[pi] = iv;
        latMap[pi]  = lv;
    }

    // Topological sort of chain.
    std::vector<Instruction*> chainOrd;
    {
        std::set<Instruction*> done(headPhis.begin(), headPhis.end());
        std::set<Instruction*> rem = chain;
        while (!rem.empty()) {
            bool prog = false;
            for (auto it = rem.begin(); it != rem.end(); ) {
                auto* inst = *it;
                bool ready = true;
                for (int k = 0; k < (int)inst->getNumOperands(); k++) {
                    auto* d = dyn_cast<Instruction>(inst->getOperand(k));
                    if (d && chain.count(d) && !done.count(d)) { ready = false; break; }
                }
                if (ready) {
                    chainOrd.push_back(inst);
                    done.insert(inst);
                    it = rem.erase(it);
                    prog = true;
                } else ++it;
            }
            if (!prog) return false;
        }
    }

    // Emit cloned chain into target block before its terminator.
    auto emitChain = [&](BasicBlock* tgt, std::unordered_map<Value*, Value*> vm)
                     -> std::pair<bool, Value*> {
        for (auto* orig : chainOrd) {
            auto* cl = cloneInst(orig, nullptr, vm);
            if (!cl) return {false, nullptr};
            cl->setParent(tgt);
            auto& ins = tgt->getInstructions();
            ins.insert(std::prev(ins.end()), cl);
            vm[orig] = cl;
        }
        Value* rc = cond;
        if (auto* d = dyn_cast<Instruction>(cond)) {
            auto it = vm.find(d);
            if (it != vm.end()) rc = it->second;
        }
        return {true, rc};
    };

    auto [pok, pcond] = emitChain(L->pre,   initMap);
    if (!pok) return false;
    auto [lok, lcond] = emitChain(L->latch, latMap);
    if (!lok) return false;

    // Rebuild full vmap (phis + chain clones) for SSA fixup.
    auto buildMap = [&](BasicBlock* tgt, std::unordered_map<Value*, Value*> vm)
                    -> std::unordered_map<Value*, Value*> {
        std::vector<Instruction*> all(tgt->getInstructions().begin(),
                                      tgt->getInstructions().end());
        int n = (int)chainOrd.size(), total = (int)all.size();
        for (int i = 0; i < n; i++)
            vm[chainOrd[i]] = all[total - 1 - n + i];
        return vm;
    };
    auto preMap = buildMap(L->pre,   initMap);
    auto latMap2 = buildMap(L->latch, latMap);

    // SSA fixup: head no longer dominates exit after rotation.
    {
        auto remap = [](Value* v, const std::unordered_map<Value*, Value*>& m) -> Value* {
            auto it = m.find(v);
            return it != m.end() ? it->second : v;
        };

        std::set<Value*> headDef;
        for (auto inst : L->head->getInstructions())
            headDef.insert(inst);

        // exit_bb phis with incoming from head.
        {
            std::vector<PhiInst*> ephis;
            for (auto inst : exit->getInstructions()) {
                auto* phi = dyn_cast<PhiInst>(inst);
                if (!phi) break;
                ephis.push_back(phi);
            }
            for (auto* phi : ephis) {
                Value* ov = nullptr;
                for (int k = 0; k < (int)phi->getNumOperands(); k += 2)
                    if (phi->getOperand(k + 1) == L->head) { ov = phi->getOperand(k); break; }
                if (!ov) continue;
                phi->removeIncomingByBlock(L->head);
                phi->addIncoming(remap(ov, preMap),  L->pre);
                phi->addIncoming(remap(ov, latMap2), L->latch);
            }
        }

        // uses of head-defined values in blocks reachable from exit.
        std::unordered_map<Value*, Value*> epCache;
        auto getEP = [&](Value* v) -> Value* {
            auto it = epCache.find(v);
            if (it != epCache.end()) return it->second;
            auto* ep = new PhiInst(cast<Instruction>(v)->getType(), nullptr);
            ep->setParent(exit);
            ep->addIncoming(remap(v, preMap),  L->pre);
            ep->addIncoming(remap(v, latMap2), L->latch);
            exit->getInstructions().insert(exit->getInstructions().begin(), ep);
            epCache[v] = ep;
            return ep;
        };

        std::set<BasicBlock*> exitReach;
        {
            std::vector<BasicBlock*> wl = {exit};
            while (!wl.empty()) {
                auto* cur = wl.back(); wl.pop_back();
                if (!exitReach.insert(cur).second) continue;
                if (cur->getInstructions().empty()) continue;
                auto* term = cur->getInstructions().back();
                if (auto* br = dyn_cast<BranchInst>(term)) {
                    int s = br->getNumOperands() == 1 ? 0 : 1;
                    for (int k = s; k < (int)br->getNumOperands(); k++)
                        if (auto* bb = dyn_cast<BasicBlock>(br->getOperand(k)))
                            wl.push_back(bb);
                }
            }
        }

        for (auto bb : f->getBody()->getBlocks()) {
            if (loopBBs.count(bb) || !exitReach.count(bb)) continue;
            std::vector<Instruction*> snap(bb->getInstructions().begin(), bb->getInstructions().end());
            for (auto* inst : snap) {
                if (auto* phi = dyn_cast<PhiInst>(inst)) {
                    for (int k = 0; k < (int)phi->getNumOperands(); k += 2)
                        if (headDef.count(phi->getOperand(k)))
                            phi->setOperand(k, getEP(phi->getOperand(k)));
                } else {
                    for (int k = 0; k < (int)inst->getNumOperands(); k++) {
                        Value* op = inst->getOperand(k);
                        if (headDef.count(op))
                            inst->setOperand(k, getEP(op));
                    }
                }
            }
        }
    }

    // Rewrite terminators.
    { auto& ins = L->pre->getInstructions(); ins.erase(std::prev(ins.end())); new BranchInst(pcond, L->head, exit, L->pre); }
    { auto& ins = L->head->getInstructions(); ins.erase(std::prev(ins.end())); new BranchInst(body, L->head); }
    { auto& ins = L->latch->getInstructions(); ins.erase(std::prev(ins.end())); new BranchInst(lcond, L->head, exit, L->latch); }

    return true;
}

bool LICM::isFullyOuterInvariant(Loop* outer, Loop* inner) {
    std::set<BasicBlock*> outerBBs(outer->blocks.begin(), outer->blocks.end());
    std::set<BasicBlock*> innerBBs(inner->blocks.begin(), inner->blocks.end());

    // Collect write-bases from outer-only blocks; bail on impure calls.
    std::set<Value*> owb;
    for (auto bb : outerBBs) {
        if (innerBBs.count(bb)) continue;
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst))
                owb.insert(getBaseObject(st->getOperand(1)));
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isPureFunc(call->getFunction(), purityCache))
                    return false;
        }
    }

    for (auto bb : inner->blocks) {
        for (auto inst : bb->getInstructions()) {
            if (auto* call = dyn_cast<CallInst>(inst))
                if (!isPureFunc(call->getFunction(), purityCache))
                    return false;
            // No operand may be defined in an outer-only block.
            for (int k = 0; k < (int)inst->getNumOperands(); k++) {
                auto* def = dyn_cast<Instruction>(inst->getOperand(k));
                if (def && outerBBs.count(def->getParent()) && !innerBBs.count(def->getParent()))
                    return false;
            }
            if (auto* ld = dyn_cast<LoadInst>(inst))
                if (owb.count(getBaseObject(ld->getOperand(0)))) return false;
            if (auto* st = dyn_cast<StoreInst>(inst))
                if (owb.count(getBaseObject(st->getOperand(1)))) return false;
        }
    }
    return true;
}

bool LICM::tryHoistSubloop(Loop* outer) {
    for (auto inner : outer->sub)
        if (tryHoistSubloop(inner)) return true;

    // Fold single-operand phis whose value is defined before the outer loop.
    {
        std::set<BasicBlock*> outerSet(outer->blocks.begin(), outer->blocks.end());
        bool folded;
        do {
            folded = false;
            for (auto bb : outer->blocks) {
                auto& ins = bb->getInstructions();
                for (auto it = ins.begin(); it != ins.end(); ) {
                    auto* phi = dyn_cast<PhiInst>(*it);
                    if (!phi || phi->getNumOperands() != 2) { ++it; continue; }
                    Value* val = phi->getOperand(0);
                    auto* def = dyn_cast<Instruction>(val);
                    if (def && outerSet.count(def->getParent())) { ++it; continue; }
                    phi->replaceAllUsesWith(val);
                    it = ins.erase(it);
                    folded = true;
                }
            }
        } while (folded);
    }

    for (auto inner : outer->sub) {
        if (!inner->latch || !inner->pre || !outer->pre) continue;

        // Reject multi-latch or multi-exit inner loops.
        {
            std::set<BasicBlock*> iset(inner->blocks.begin(), inner->blocks.end());
            int lc = 0;
            std::set<BasicBlock*> exits;
            for (auto bb : inner->blocks) {
                auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
                if (!br) continue;
                if (br->getNumOperands() == 1) {
                    auto* s = cast<BasicBlock>(br->getOperand(0));
                    if (s == inner->head) lc++;
                    else if (!iset.count(s)) exits.insert(s);
                } else if (br->getNumOperands() == 3) {
                    auto* s1 = cast<BasicBlock>(br->getOperand(1));
                    auto* s2 = cast<BasicBlock>(br->getOperand(2));
                    if (s1 == inner->head) lc++;
                    else if (!iset.count(s1)) exits.insert(s1);
                    if (s2 == inner->head) lc++;
                    else if (!iset.count(s2)) exits.insert(s2);
                }
            }
            if (lc > 1 || exits.size() > 1) continue;
        }

        if (!isFullyOuterInvariant(outer, inner)) continue;

        auto* ibr = dyn_cast<BranchInst>(inner->head->getInstructions().back());
        if (!ibr || ibr->getNumOperands() != 3) continue;
        BasicBlock* t1 = cast<BasicBlock>(ibr->getOperand(1));
        BasicBlock* t2 = cast<BasicBlock>(ibr->getOperand(2));
        BasicBlock* iexit = inner->has(t1) ? t2 : t1;

        auto* opbr = dyn_cast<BranchInst>(outer->pre->getInstructions().back());
        if (!opbr || opbr->getNumOperands() != 1) continue;
        if (cast<BasicBlock>(opbr->getOperand(0)) != outer->head) continue;

        auto* ipbr = dyn_cast<BranchInst>(inner->pre->getInstructions().back());
        if (!ipbr || ipbr->getNumOperands() != 1) continue;
        if (cast<BasicBlock>(ipbr->getOperand(0)) != inner->head) continue;

        // CFG transformation: hoist inner loop before outer loop.
        Region* region = outer->pre->getParent();
        auto* npre = new BasicBlock("pre_" + outer->head->getName() + "_h", region);
        new BranchInst(outer->head, npre);
        auto& blist = region->getBlocks();
        blist.splice(std::find(blist.begin(), blist.end(), outer->head),
                     blist, std::prev(blist.end()));

        opbr->replaceSuccessor(outer->head, inner->head); // outer_pre -> inner_head
        ibr->replaceSuccessor(iexit, npre);                // inner exits -> new outer_pre
        ipbr->replaceSuccessor(inner->head, iexit);        // inner_pre bypasses -> inner_exit

        // Fix phis: inner_head preheader -> outer_pre; outer_head preheader -> new_pre.
        for (auto inst : inner->head->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst); if (!phi) break;
            for (int k = 1; k < (int)phi->getNumOperands(); k += 2)
                if (phi->getOperand(k) == inner->pre) phi->setOperand(k, outer->pre);
        }
        for (auto inst : outer->head->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst); if (!phi) break;
            for (int k = 1; k < (int)phi->getNumOperands(); k += 2)
                if (phi->getOperand(k) == outer->pre) phi->setOperand(k, npre);
        }
        return true;
    }
    return false;
}

// Domtree DFS LICM. hoistable flag: starts true; Load/Branch sets it false,
// preventing stores in the same domtree subtree from being hoisted.
bool LICM::hoistLoop(Loop* L, Dominators& dt, SCEV& scev) {
    if (!L->pre || !L->latch) return false;
    {
        auto* lbr = dyn_cast<BranchInst>(L->latch->getInstructions().back());
        if (!lbr || lbr->getNumOperands() != 3) return false;
    }

    std::vector<StoreInst*> stores;
    bool impure = false, hasMW = false;
    for (auto bb : L->blocks) {
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst)) stores.push_back(st);
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (!isPureFunc(call->getFunction(), purityCache)) {
                    impure = true;
                    if (!isReadOnlyFunc(call->getFunction(), readOnlyCache))
                        hasMW = true;
                }
            }
        }
    }

    std::map<BasicBlock*, std::vector<BasicBlock*>> domCh;
    for (auto bb : L->blocks) domCh[bb] = {};
    for (auto bb : L->blocks) {
        if (bb == L->head) continue;
        auto* idom = dt.getIDom(bb);
        if (idom && domCh.count(idom)) domCh[idom].push_back(bb);
    }

    std::set<Instruction*> inv;
    auto outside = [&](Value* v) -> bool {
        if (!isa<Instruction>(v)) return true;
        auto* i = cast<Instruction>(v);
        return !L->has(i->getParent()) || inv.count(i);
    };

    auto& pre = L->pre->getInstructions();
    auto ins_pt = std::prev(pre.end());
    bool any = false;

    std::function<void(BasicBlock*, bool)> visit = [&](BasicBlock* bb, bool hoistable) {
        std::vector<Instruction*> toHoist;

        for (auto inst : bb->getInstructions()) {
            auto op = inst->getOpID();

            if (op == Instruction::Load || op == Instruction::Br)
                hoistable = false;

            if (op == Instruction::Br  || op == Instruction::Ret ||
                op == Instruction::Phi || op == Instruction::Alloca)
                continue;

            if (op == Instruction::Call) {
                auto* call = cast<CallInst>(inst);
                if (!isPureFunc(call->getFunction(), purityCache)) continue;
                bool ok = true;
                for (int i = 1; i < (int)inst->getNumOperands(); i++)
                    if (!outside(inst->getOperand(i))) { ok = false; break; }
                if (ok) { inv.insert(inst); toHoist.push_back(inst); }
                continue;
            }

            if (op == Instruction::Store) {
                if (!hoistable || impure) continue;
                if (!outside(inst->getOperand(0)) || !outside(inst->getOperand(1))) continue;
                inv.insert(inst); toHoist.push_back(inst);
                continue;
            }

            if (op == Instruction::Load) {
                if (hasMW || !outside(inst->getOperand(0))) continue;
                Value* lb = getBaseObject(inst->getOperand(0));
                SE* lse = scev.get(inst->getOperand(0));
                bool alias = false;
                for (auto* st : stores) {
                    if (inv.count(st)) continue;
                    if (getBaseObject(st->getOperand(1)) != lb) continue;
                    if (!scev.distinct(lse, scev.get(st->getOperand(1)))) { alias = true; break; }
                }
                if (!alias) { inv.insert(inst); toHoist.push_back(inst); }
                continue;
            }

            bool ok = true;
            for (int i = 0; i < (int)inst->getNumOperands(); i++)
                if (!outside(inst->getOperand(i))) { ok = false; break; }
            if (ok) { inv.insert(inst); toHoist.push_back(inst); }
        }

        for (auto* inst : toHoist) {
            bb->getInstructions().remove(inst);
            inst->setParent(L->pre);
            pre.insert(ins_pt, inst);
        }
        any |= !toHoist.empty();

        for (auto* child : domCh[bb])
            visit(child, hoistable);
    };

    visit(L->head, true);
    return any;
}
