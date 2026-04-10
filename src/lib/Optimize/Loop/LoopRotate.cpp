#include "Optimize/Loop/LoopRotate.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <functional>
#include <set>
#include <unordered_map>

using namespace sysy;

static int LoopRotateID = 0;

static std::string LoopName(const std::string& seed) {
    if (!seed.empty())
        return seed + ".lr" + std::to_string(LoopRotateID++);
    return "%lr" + std::to_string(LoopRotateID++);
}

static void AssignName(Instruction* inst, const std::string& seed = "") {
    if (!inst || inst->getType()->isVoid()) return;
    inst->setName(LoopName(seed));
}

static Value* GetBase(Value* v, std::set<Value*>& vis) {
    if (!vis.insert(v).second) return v;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return GetBase(gep->getOperand(0), vis);
    if (auto* phi = dyn_cast<PhiInst>(v)) {
        Value* base = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            Value* b = GetBase(phi->getOperand(k), vis);
            if (!base) base = b;
            else if (base != b) return v;
        }
        return base ? base : v;
    }
    return v;
}

static Value* GetBase(Value* v) {
    std::set<Value*> vis;
    return GetBase(v, vis);
}

static bool isSpeculateLoad(LoadInst* load, Loop* L,
                                        std::unordered_map<Function*, bool>& purityCache) {
    if (!load || !L) return false;
    Value* base = GetBase(load->getOperand(0));
    if (!isa<GlobalVariable>(base) && !isa<AllocaInst>(base) && !isa<Argument>(base))
        return false;

    for (auto* bb : L->blocks) {
        for (auto* inst : bb->getInstructions()) {
            if (inst == load) continue;
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                if (GetBase(st->getOperand(1)) == base)
                    return false;
            } else if (auto* call = dyn_cast<CallInst>(inst)) {
                if (!isPureFunc(call->getFunction(), purityCache))
                    return false;
            }
        }
    }
    return true;
}

static bool DominatesEdge(Value* val, BasicBlock* fromBB, Dominators& dt) {
    if (!val) return false;
    if (isa<Constant>(val) || isa<Argument>(val) || isa<GlobalVariable>(val) ||
        isa<Function>(val) || isa<BasicBlock>(val)) {
        return true;
    }

    auto* def = dyn_cast<Instruction>(val);
    if (!def || !def->getParent()) return false;
    if (def->getParent() == fromBB) return true;
    return dt.dominates(def->getParent(), fromBB);
}

static bool dominatesLoopRotateUse(Value* val, Instruction* userInst, int operandIndex,
                                    Dominators& dt) {
    if (!val) return false;
    if (isa<Constant>(val) || isa<Argument>(val) || isa<GlobalVariable>(val) ||
        isa<Function>(val) || isa<BasicBlock>(val)) {
        return true;
    }

    auto* def = dyn_cast<Instruction>(val);
    if (!def || !def->getParent() || !userInst || !userInst->getParent()) return false;

    if (auto* phi = dyn_cast<PhiInst>(userInst)) {
        if (operandIndex % 2 == 1) return true;
        if (operandIndex + 1 >= phi->getNumOperands()) return false;
        auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(operandIndex + 1));
        if (!fromBB) return false;
        return DominatesEdge(val, fromBB, dt);
    }

    BasicBlock* defBB = def->getParent();
    BasicBlock* useBB = userInst->getParent();
    if (defBB != useBB)
        return dt.dominates(defBB, useBB);

    for (auto* inst : useBB->getInstructions()) {
        if (inst == def) return true;
        if (inst == userInst) return false;
    }
    return false;
}

// Clone an arithmetic/comparison/cast/GEP instruction, remapping operands via vmap.
// Returns nullptr for unsupported opcodes (caller should abort the rotation).
static Instruction* cloneInst(Instruction* inst,
                               const std::unordered_map<Value*, Value*>& vmap) {
    auto remap = [&](Value* v) -> Value* {
        auto it = vmap.find(v);
        return it != vmap.end() ? it->second : v;
    };
    auto op = inst->getOpID();
    Instruction* clone = nullptr;
    if (isa<BinaryInst>(inst))
        clone = new BinaryInst(op, remap(inst->getOperand(0)),
                            remap(inst->getOperand(1)), nullptr);
    else if (auto* ic = dyn_cast<ICmpInst>(inst))
        clone = new ICmpInst(ic->getPredicate(), remap(inst->getOperand(0)),
                            remap(inst->getOperand(1)), nullptr);
    else if (auto* fc = dyn_cast<FCmpInst>(inst))
        clone = new FCmpInst(fc->getPredicate(), remap(inst->getOperand(0)),
                            remap(inst->getOperand(1)), nullptr);
    else if (isa<CastInst>(inst))
        clone = new CastInst(op, remap(inst->getOperand(0)), inst->getType(), nullptr);
    else if (isa<LoadInst>(inst)) {
        Value* origPtr = inst->getOperand(0);
        clone = new LoadInst(origPtr, nullptr);
        clone->setOperand(0, remap(origPtr));
    }
    else if (isa<GetElementPtrInst>(inst))
        clone = new GetElementPtrInst(remap(inst->getOperand(0)),
                                    remap(inst->getOperand(1)), nullptr);

    if (clone)
        AssignName(clone, inst->getName());
    return clone;
}

bool LoopRotate::runOnLoop(Loop* L, Function* f) {
    // LoopSimplify must have run: unique preheader + single latch required.
    if (!L->head || !L->pre || L->latches.size() != 1) return false;

    // latch must be an unconditional back-edge to head.
    auto* lbr = dyn_cast<BranchInst>(L->latch->getInstructions().back());
    if (!lbr || lbr->getNumOperands() != 1) return false;
    if (cast<BasicBlock>(lbr->getOperand(0)) != L->head) return false;

    // head must end with a conditional branch: one target in loop (body), one out (exit).
    auto* hbr = dyn_cast<BranchInst>(L->head->getInstructions().back());
    if (!hbr || hbr->getNumOperands() != 3) return false;
    BasicBlock* t1 = cast<BasicBlock>(hbr->getOperand(1));
    BasicBlock* t2 = cast<BasicBlock>(hbr->getOperand(2));
    std::set<BasicBlock*> loopBBs(L->blocks.begin(), L->blocks.end());
    if (loopBBs.count(t1) == loopBBs.count(t2)) return false;
    BasicBlock* body = loopBBs.count(t1) ? t1 : t2;
    BasicBlock* exit = loopBBs.count(t1) ? t2 : t1;

    // pre must be an unconditional branch to head.
    auto* pbr = dyn_cast<BranchInst>(L->pre->getInstructions().back());
    if (!pbr || pbr->getNumOperands() != 1) return false;
    if (cast<BasicBlock>(pbr->getOperand(0)) != L->head) return false;

    Value* cond = hbr->getOperand(0);
    if (!cond) return false; // while(1): condition folded away

    // Collect head phis and the condition-computation chain.
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

    // Refuse to speculate unsafe instructions.
    std::unordered_map<Function*, bool> purityCache;
    for (auto* inst : chain) {
        if (isa<StoreInst>(inst) || isa<CallInst>(inst))
            return false;
        if (auto* load = dyn_cast<LoadInst>(inst)) {
            if (!isSpeculateLoad(load, L, purityCache))
                return false;
        }
    }

    // Head must contain only phis + chain + branch.
    for (auto inst : L->head->getInstructions()) {
        if (isa<PhiInst>(inst) || isa<BranchInst>(inst) || chain.count(inst)) continue;
        return false;
    }

    // Build phi → {init value from pre, iter value from latch}.
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

    // Topological sort of the condition chain (respecting intra-chain deps).
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

    // Emit a clone of the condition chain into tgt before its terminator.
    auto emitChain = [&](BasicBlock* tgt, std::unordered_map<Value*, Value*> vm)
                     -> std::pair<bool, Value*> {
        for (auto* orig : chainOrd) {
            auto* cl = cloneInst(orig, vm);
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

    auto preChainRes = emitChain(L->pre,   initMap);
    if (!preChainRes.first) return false;
    Value* pcond = preChainRes.second;

    auto latChainRes = emitChain(L->latch, latMap);
    if (!latChainRes.first) return false;
    Value* lcond = latChainRes.second;

    // Rebuild full vmap (phis + clones) for SSA fixup.
    auto buildMap = [&](BasicBlock* tgt, std::unordered_map<Value*, Value*> vm)
                    -> std::unordered_map<Value*, Value*> {
        std::vector<Instruction*> all(tgt->getInstructions().begin(),
                                        tgt->getInstructions().end());
        int n = (int)chainOrd.size(), total = (int)all.size();
        for (int i = 0; i < n; i++)
            vm[chainOrd[i]] = all[total - 1 - n + i];
        return vm;
    };
    auto preMap  = buildMap(L->pre,   initMap);
    auto latMap2 = buildMap(L->latch, latMap);

    auto remap = [](Value* v, const std::unordered_map<Value*, Value*>& m) -> Value* {
        auto it = m.find(v);
        return it != m.end() ? it->second : v;
    };

    std::set<Value*> headDef;
    for (auto inst : L->head->getInstructions())
        headDef.insert(inst);

    std::vector<BasicBlock*> bodyExitPreds;
    for (auto* bb : loopBBs) {
        if (bb == L->head || bb == L->latch) continue;
        auto& insts = bb->getInstructions();
        if (insts.empty()) continue;
        auto* br = dyn_cast<BranchInst>(insts.back());
        if (!br) continue;
        int s = br->getNumOperands() == 1 ? 0 : 1;
        for (int k = s; k < (int)br->getNumOperands(); k++) {
            if (dyn_cast<BasicBlock>(br->getOperand(k)) == exit) {
                bodyExitPreds.push_back(bb);
                break;
            }
        }
    }

    std::unordered_map<Value*, Value*> epCache;
    auto getEP = [&](Value* v) -> Value* {
        auto it = epCache.find(v);
        if (it != epCache.end()) return it->second;
        auto* ep = new PhiInst(cast<Instruction>(v)->getType(), nullptr);
        ep->setParent(exit);
        AssignName(ep, v->getName());
        ep->addIncoming(remap(v, preMap),  L->pre);
        ep->addIncoming(remap(v, latMap2), L->latch);
        // v is a chain instruction defined in head,
        // head dominates all body blocks, so for each body exit predecessor we use v directly.
        for (auto* pred : bodyExitPreds)
            ep->addIncoming(cast<Instruction>(v), pred);
        exit->getInstructions().insert(exit->getInstructions().begin(), ep);
        epCache[v] = ep;
        return ep;
    };

    std::unordered_map<PhiInst*, Value*> loopPhiExitCache;
    auto getLoopPhiExit = [&](PhiInst* phi) -> Value* {
        auto it = loopPhiExitCache.find(phi);
        if (it != loopPhiExitCache.end()) return it->second;

        Value* preVal = nullptr;
        Value* latVal = nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* from = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (!from) continue;
            if (from == L->latch || loopBBs.count(from))
                latVal = phi->getOperand(k);
            else
                preVal = phi->getOperand(k);
        }
        if (!preVal || !latVal) return nullptr;

        auto* ep = new PhiInst(phi->getType(), nullptr);
        ep->setParent(exit);
        AssignName(ep, phi->getName());
        ep->addIncoming(remap(preVal, preMap),  L->pre);
        ep->addIncoming(remap(latVal, latMap2), L->latch);
        // phi is a header phi defined in head; head dominates all body blocks,
        // so for each body exit predecessor the current iteration value is phi itself.
        for (auto* pred : bodyExitPreds)
            ep->addIncoming(phi, pred);
        exit->getInstructions().insert(exit->getInstructions().begin(), ep);
        loopPhiExitCache[phi] = ep;
        return ep;
    };

    // Rewrite terminators.
    auto replaceTerminator = [](BasicBlock* bb, auto makeNew) {
        auto& ins = bb->getInstructions();
        auto it = std::prev(ins.end());
        delete *it;
        ins.erase(it);
        makeNew();
    };
    replaceTerminator(L->pre, [&]{ new BranchInst(pcond, L->head, exit, L->pre); });
    replaceTerminator(L->head, [&]{ new BranchInst(body, L->head); });
    replaceTerminator(L->latch, [&]{ new BranchInst(lcond, L->head, exit, L->latch); });

    // SSA fixup after CFG rewrite: repair exit phis and any remaining undominated
    // uses of loop-local values in blocks reachable from exit.
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
            phi->addIncoming(remap(ov, preMap), L->pre);
            phi->addIncoming(remap(ov, latMap2), L->latch);
        }

        std::set<BasicBlock*> exitReach;
        std::vector<BasicBlock*> wl = {exit};
        while (!wl.empty()) {
            auto* cur = wl.back();
            wl.pop_back();
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

        Dominators rotDt(f);
        rotDt.run();

        for (auto bb : f->getBody()->getBlocks()) {
            if (loopBBs.count(bb) || !exitReach.count(bb)) continue;
            std::vector<Instruction*> snap(bb->getInstructions().begin(),
                                            bb->getInstructions().end());
            for (auto* inst : snap) {
                if (auto* phi = dyn_cast<PhiInst>(inst)) {
                    for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
                        Value* op = phi->getOperand(k);
                        if (dominatesLoopRotateUse(op, phi, k, rotDt)) continue;
                        if (headDef.count(op))
                            phi->setOperand(k, getEP(op));
                        else if (auto* loopPhi = dyn_cast<PhiInst>(op)) {
                            if (loopBBs.count(loopPhi->getParent())) {
                                if (auto* ep = getLoopPhiExit(loopPhi))
                                    phi->setOperand(k, ep);
                            }
                        }
                    }
                } else {
                    for (int k = 0; k < (int)inst->getNumOperands(); k++) {
                        Value* op = inst->getOperand(k);
                        if (dominatesLoopRotateUse(op, inst, k, rotDt)) continue;
                        if (headDef.count(op))
                            inst->setOperand(k, getEP(op));
                        else if (auto* loopPhi = dyn_cast<PhiInst>(op)) {
                            if (loopBBs.count(loopPhi->getParent())) {
                                if (auto* ep = getLoopPhiExit(loopPhi))
                                    inst->setOperand(k, ep);
                            }
                        }
                    }
                }
            }
        }
    }

    // if (f->getName() == "heap_sort") {
    //     std::cerr << "==== after one LoopRotate on heap_sort ====\n";
    //     std::cerr << f->toString() << "\n";
    // }


    return true;
}

bool LoopRotate::runOnFunction(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    bool changed = false;
    bool rot;
    do {
        rot = false;
        Dominators dt(f); dt.run();
        LoopInfo li(f, dt);
        std::function<bool(Loop*)> visit = [&](Loop* L) -> bool {
            for (auto sub : L->sub) if (visit(sub)) return true;
            return runOnLoop(L, f);
        };
        for (auto top : li.tops())
            if (visit(top)) { changed = rot = true; break; }
    } while (rot);
    return changed;
}

bool LoopRotate::run() {
    bool any = false;
    for (auto f : M->getFunctions())
        any |= runOnFunction(f);
    return any;
}
