#include "../../../include/Optimize/Loop/LoopGVN.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/LoopInfo.h"
#include "../../../include/Optimize/Analysis/SCEV.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopAliasUtils.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "../../../include/IR/Instruction.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace sysy;

static bool collectTree(Value* v,
                        const std::unordered_set<BasicBlock*>& L1Blocks,
                        const std::map<Value*, Value*>& ivRemap,
                        std::vector<Instruction*>& order,
                        std::set<Instruction*>& seen) {
    if (ivRemap.count(v)) return true;
    auto* inst = dyn_cast<Instruction>(v);
    if (!inst || !L1Blocks.count(inst->getParent())) return true;
    if (seen.count(inst)) return true;

    switch (inst->getOpID()) {
    case Instruction::Add: case Instruction::Sub:
    case Instruction::Mul: case Instruction::Div: case Instruction::Mod:
    case Instruction::FAdd: case Instruction::FSub:
    case Instruction::FMul: case Instruction::FDiv:
    case Instruction::SIToFP: case Instruction::FPToSI:
    case Instruction::ICmp: case Instruction::FCmp:
    case Instruction::GetElementPtr:
        break;
    default:
        return false;
    }

    seen.insert(inst);
    for (int i = 0; i < inst->getNumOperands(); i++)
        if (!collectTree(inst->getOperand(i), L1Blocks, ivRemap, order, seen))
            return false;
    order.push_back(inst);
    return true;
}

static Value* emitClones(Value* val,
                        const std::vector<Instruction*>& order,
                        const std::map<Value*, Value*>& ivRemap,
                        Instruction* insertBefore) {
    std::map<Value*, Value*> memo(ivRemap.begin(), ivRemap.end());
    auto remap = [&](Value* v) -> Value* {
        auto it = memo.find(v);
        return it != memo.end() ? it->second : v;
    };

    BasicBlock* bb = insertBefore->getParent();
    auto& list = bb->getInstructions();
    auto pos = std::find(list.begin(), list.end(), insertBefore);

    for (auto* src : order) {
        Instruction* c = nullptr;
        switch (src->getOpID()) {
        case Instruction::Add:  case Instruction::Sub:
        case Instruction::Mul:  case Instruction::Div:  case Instruction::Mod:
        case Instruction::FAdd: case Instruction::FSub:
        case Instruction::FMul: case Instruction::FDiv:
            c = new BinaryInst(src->getOpID(),
                                remap(src->getOperand(0)),
                                remap(src->getOperand(1)), nullptr);
            break;
        case Instruction::SIToFP: case Instruction::FPToSI:
            c = new CastInst(src->getOpID(), remap(src->getOperand(0)),
                            src->getType(), nullptr);
            break;
        case Instruction::ICmp:
            c = new ICmpInst(cast<ICmpInst>(src)->getPredicate(),
                            remap(src->getOperand(0)),
                            remap(src->getOperand(1)), nullptr);
            break;
        case Instruction::FCmp:
            c = new FCmpInst(cast<FCmpInst>(src)->getPredicate(),
                            remap(src->getOperand(0)),
                            remap(src->getOperand(1)), nullptr);
            break;
        case Instruction::GetElementPtr:
            c = new GetElementPtrInst(remap(src->getOperand(0)),
                                    remap(src->getOperand(1)), nullptr);
            cast<GetElementPtrInst>(c)->setIndexedType(
                cast<GetElementPtrInst>(src)->getIndexedType());
            break;
        default:
            return nullptr;
        }
        c->setName(src->getName() + ".lgvn");
        c->setParent(bb);
        list.insert(pos, c);
        memo[src] = c;
    }
    return remap(val);
}

struct StoreEntry {
    StoreInst* inst;
    SE* start;
    int64_t step;
    Value* val;
    Value* base;
};

// True if L has any store not in stores, or any call.
static bool hasSideEffect(Loop* L, const std::vector<StoreEntry>& stores) {
    std::set<StoreInst*> known;
    for (auto& e : stores) known.insert(e.inst);
    for (auto* bb : L->blocks)
        for (auto* inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                if (!known.count(st)) return true;
            } else if (isa<CallInst>(inst)) {
                return true;
            }
        }
    return false;
}

// True if it is safe to bypass L after forwarding: all stores matched,
// no side effects, bases are local allocas, no remaining load/call touches them.
static bool canBypass(Function* f, Loop* L,
                    const std::vector<StoreEntry>& stores,
                    const std::set<int>& matched,
                    const std::set<LoadInst*>& forwarded) {
    if (stores.empty() || matched.size() != stores.size()) return false;
    if (hasSideEffect(L, stores)) return false;

    std::set<Value*> bases;
    for (auto& e : stores) {
        if (!isa<AllocaInst>(e.base)) return false;
        bases.insert(e.base);
    }

    for (auto* bb : f->getBody()->getBlocks())
        for (auto* inst : bb->getInstructions()) {
            if (auto* ld = dyn_cast<LoadInst>(inst)) {
                if (!forwarded.count(ld) &&
                    bases.count(getLoopBaseObject(ld->getOperand(0))))
                    return false;
            }
            if (auto* call = dyn_cast<CallInst>(inst))
                for (int i = 1; i < call->getNumOperands(); ++i)
                    if (bases.count(getLoopBaseObject(call->getOperand(i))))
                        return false;
        }
    return true;
}

static bool hasExternalStore(Function* f, Loop* L,
                            const std::vector<StoreEntry>& stores) {
    std::set<Value*> bases;
    for (auto& e : stores) bases.insert(e.base);
    for (auto* bb : f->getBody()->getBlocks()) {
        if (L->has(bb)) continue;
        for (auto* inst : bb->getInstructions()) {
            auto* st = dyn_cast<StoreInst>(inst);
            if (st && bases.count(getLoopBaseObject(st->getOperand(1))))
                return true;
        }
    }
    return false;
}

// Forward stores from L1 into matching loads in L2 when L1.exit dom L2.head
// and both loops have the same constant trip count.
static bool tryFuse(Loop* L1, Loop* L2, SCEV& scev, Dominators& dt) {
    if (!L1->pre || !L1->latch || !L2->pre || !L2->latch) return false;
    if (L1->exits.size() != 1) return false;
    if (!dt.dominates(L1->exits[0], L2->head)) return false;

    ExitBranchInfo info1, info2;
    int64_t tc1 = -1, tc2 = -1;
    if (!analyzeExitBranch(L1, L1->latch, scev, info1) ||
        !getConstantTripCountFromInfo(info1, L1, tc1) || tc1 <= 0)
        return false;
    if (!analyzeExitBranch(L2, L2->latch, scev, info2) ||
        !getConstantTripCountFromInfo(info2, L2, tc2) || tc2 <= 0)
        return false;
    if (tc1 != tc2) return false;

    // Map L1 IVs -> L2 IVs with matching SCEV (same start + step).
    std::map<Value*, Value*> ivRemap;
    for (auto* i1 : L1->head->getInstructions()) {
        auto* phi1 = dyn_cast<PhiInst>(i1);
        if (!phi1) break;
        auto* ar1 = dyn_cast<SEAddRec>(scev.get(phi1));
        if (!ar1 || ar1->loop != L1) continue;
        for (auto* i2 : L2->head->getInstructions()) {
            auto* phi2 = dyn_cast<PhiInst>(i2);
            if (!phi2) break;
            auto* ar2 = dyn_cast<SEAddRec>(scev.get(phi2));
            if (!ar2 || ar2->loop != L2) continue;
            if (ar2->step == ar1->step && scev.equal(ar2->start, ar1->start)) {
                ivRemap[phi1] = phi2;
                break;
            }
        }
    }

    // Collect SCEV-addressable stores from L1.
    std::vector<StoreEntry> stores;
    for (auto* bb : L1->blocks)
        for (auto* inst : bb->getInstructions()) {
            auto* st = dyn_cast<StoreInst>(inst);
            if (!st) continue;
            auto* ar = dyn_cast<SEAddRec>(scev.get(st->getOperand(1)));
            if (!ar || ar->loop != L1) continue;
            stores.push_back({st, ar->start, ar->step, st->getOperand(0),
                            getLoopBaseObject(st->getOperand(1))});
        }
    if (stores.empty()) return false;

    Function* func = L1->head ? L1->head->getParentFunc() : nullptr;
    if (!func || hasExternalStore(func, L1, stores)) return false;

    // Replace matching loads in L2 with cloned producer expressions.
    bool changed = false;
    std::set<LoadInst*> forwarded;
    std::set<int> matched;
    for (auto* bb : L2->blocks) {

        struct Pending { 
            LoadInst* ld; 
            int si; 
            Value* val; 
            std::vector<Instruction*> order; 
        };

        std::vector<Pending> pending;

        for (auto* inst : bb->getInstructions()) {
            auto* ld = dyn_cast<LoadInst>(inst);
            if (!ld) continue;
            auto* ar = dyn_cast<SEAddRec>(scev.get(ld->getOperand(0)));
            if (!ar || ar->loop != L2) continue;

            for (int si = 0; si < (int)stores.size(); ++si) {
                auto& e = stores[si];
                if (e.step != ar->step) continue;
                if (!scev.equal(e.start, ar->start)) continue;
                std::vector<Instruction*> order;
                std::set<Instruction*> seen;
                if (!collectTree(e.val, L1->blockSet, ivRemap, order, seen)) continue;
                pending.push_back({ld, si, e.val, std::move(order)});
                break;
            }
        }

        for (auto& p : pending) {
            Value* repl = emitClones(p.val, p.order, ivRemap, p.ld);
            if (!repl) continue;
            forwarded.insert(p.ld);
            matched.insert(p.si);
            p.ld->replaceAllUsesWith(repl);
            p.ld->eraseInst();
            changed = true;
        }
    }

    // Bypass L1 if all its stores were forwarded and no other code reads them.
    if (changed && canBypass(func, L1, stores, matched, forwarded)) {
        BasicBlock* exitBB = L1->exits[0];
        BasicBlock* pre = L1->pre;
        if (pre && !pre->getInstructions().empty()) {
            auto* br = dyn_cast<BranchInst>(pre->getInstructions().back());
            int opIdx = -1;
            if (br) {
                if (br->getNumOperands() == 1 &&
                    cast<BasicBlock>(br->getOperand(0)) == L1->head)
                    opIdx = 0;
                else if (br->getNumOperands() == 3) {
                    if (cast<BasicBlock>(br->getOperand(1)) == L1->head) 
                        opIdx = 1;
                    else if (cast<BasicBlock>(br->getOperand(2)) == L1->head) 
                        opIdx = 2;
                }
            }
            if (opIdx != -1) {
                for (auto* inst : exitBB->getInstructions()) {
                    auto* phi = dyn_cast<PhiInst>(inst);
                    if (!phi) break;
                    bool hasPre = false;
                    for (int k = 0; k < phi->getNumOperands(); k += 2)
                        if (cast<BasicBlock>(phi->getOperand(k + 1)) == pre) { 
                            hasPre = true; 
                            break;
                        }
                    std::vector<std::pair<Value*, BasicBlock*>> rem;
                    for (int k = 0; k < phi->getNumOperands(); k += 2) {
                        auto* from = cast<BasicBlock>(phi->getOperand(k + 1));
                        if (L1->has(from)) rem.push_back({phi->getOperand(k), from});
                    }
                    for (auto& [val, from] : rem) {
                        phi->removeIncomingByBlock(from);
                        if (!hasPre) { 
                            phi->addIncoming(val, pre); 
                            hasPre = true; 
                        }
                    }
                }
                br->setOperand(opIdx, exitBB);
            }
        }
    }

    return changed;
}

bool LoopGVN::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    Dominators dt(f); dt.run();
    LoopInfo li(f, dt);
    SCEV scev(f, li);

    std::vector<Loop*> loops;
    std::function<void(Loop*)> collect = [&](Loop* L) {
        loops.push_back(L);
        for (auto* sub : L->sub) collect(sub);
    };
    for (auto* top : li.tops()) collect(top);

    bool changed = false;
    for (size_t i = 0; i < loops.size(); i++)
        for (size_t j = 0; j < loops.size(); j++)
            if (i != j) changed |= tryFuse(loops[i], loops[j], scev, dt);
    return changed;
}

bool LoopGVN::run() {
    bool any = false;
    for (auto* f : M->getFunctions())
        any |= runFunc(f);
    return any;
}
