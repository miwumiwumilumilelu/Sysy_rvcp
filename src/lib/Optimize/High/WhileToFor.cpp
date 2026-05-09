#include "Optimize/High/WhileToFor.h"
#include "IR/IRRewriter.h"
#include "Optimize/Analysis/EffectAnalysis.h"
#include "IR/Instruction.h"
#include <algorithm>
#include <functional>
#include <set>
#include <vector>

using namespace sysy;

namespace {

static bool legalICmp(Instruction* inst) {
    auto* ic = dyn_cast<ICmpInst>(inst);
    if (!ic) return false;
    auto p = ic->getPredicate();
    return p == ICmpInst::SLT || p == ICmpInst::SLE ||
            p == ICmpInst::SGT || p == ICmpInst::SGE;
}

// break only affect the inner while in nested WhileInst.
static bool hasBreak(Region* region) {
    if (!region) return false;
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            if (isa<BreakInst>(inst))
                return true;
            if (auto* ii = dyn_cast<IfInst>(inst)) {
                if (hasBreak(ii->getThenRegion())) return true;
                if (ii->getElseRegion() && hasBreak(ii->getElseRegion())) return true;
            }
            // skip whileInst.
        }
    }
    return false;
}

// Collect all ContinueInst in region recursively through IfInst.
static void collectContinues(Region* region, std::vector<ContinueInst*>& out) {
    if (!region) return;
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            if (auto* ci = dyn_cast<ContinueInst>(inst))
                out.push_back(ci);
            else if (auto* ii = dyn_cast<IfInst>(inst)) {
                collectContinues(ii->getThenRegion(), out);
                if (ii->getElseRegion())
                    collectContinues(ii->getElseRegion(), out);
            }
            // Skip WhileInst.
        }
    }
}

// Check inst is a canonical IV store: store(load(ivAddr) +/- step, ivAddr).
static bool isIVRec(Instruction* inst, Value* ivAddr, int step) {
    auto* st = dyn_cast<StoreInst>(inst);
    if (!st || st->getOperand(1) != ivAddr) return false;
    auto* val = dyn_cast<BinaryInst>(st->getOperand(0));
    if (!val) return false;
    auto isIVLoad = [&](Value* v) {
        auto* ld = dyn_cast<LoadInst>(v);
        return ld && ld->getOperand(0) == ivAddr;
    };
    auto opID = val->getOpID();
    if (opID == Instruction::Add) {
        if (isIVLoad(val->getOperand(0)) && isa<ConstantInt>(val->getOperand(1)))
            return cast<ConstantInt>(val->getOperand(1))->getValue() == step;
        if (isIVLoad(val->getOperand(1)) && isa<ConstantInt>(val->getOperand(0)))
            return cast<ConstantInt>(val->getOperand(0))->getValue() == step;
    }
    // load - |step|
    if (opID == Instruction::Sub && isIVLoad(val->getOperand(0)) && isa<ConstantInt>(val->getOperand(1)))
        return -cast<ConstantInt>(val->getOperand(1))->getValue() == step;
    return false;
}

// Check if region contains any store to addr not in the allowed set.
static bool hasUnsafeStore(Region* region, Value* addr, const std::set<Instruction*>& allowed) {
    if (!region) return false;
    for (auto* bb : region->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                if (st->getOperand(1) == addr && !allowed.count(inst))
                    return true;
            } else if (auto* ii = dyn_cast<IfInst>(inst)) {
                if (hasUnsafeStore(ii->getThenRegion(), addr, allowed)) return true;
                if (ii->getElseRegion() &&
                    hasUnsafeStore(ii->getElseRegion(), addr, allowed)) 
                    return true;
            } else if (auto* wi2 = dyn_cast<WhileInst>(inst)) {
                if (hasUnsafeStore(wi2->getCondRegion(), addr, allowed) ||
                    hasUnsafeStore(wi2->getBodyRegion(), addr, allowed)) 
                    return true;
            } else if (auto* fi2 = dyn_cast<ForInst>(inst)) {
                if (hasUnsafeStore(fi2->getBodyRegion(), addr, allowed)) return true;
            }
        }
    }
    return false;
}

// Check if bodyBB's HighInst contains any store to addr not in the allowed set.
static bool hasUnsafeNestedStore(BasicBlock* bodyBB, Value* addr, const std::set<Instruction*>& allowed) {
    for (auto* inst : bodyBB->getInstructions()) {
        if (auto* ii = dyn_cast<IfInst>(inst)) {
            if (hasUnsafeStore(ii->getThenRegion(), addr, allowed)) return true;
            if (ii->getElseRegion() &&
                hasUnsafeStore(ii->getElseRegion(), addr, allowed)) 
                return true;
        } else if (auto* wi2 = dyn_cast<WhileInst>(inst)) {
            if (hasUnsafeStore(wi2->getCondRegion(), addr, allowed) ||
                hasUnsafeStore(wi2->getBodyRegion(), addr, allowed)) 
                return true;
        } else if (auto* fi2 = dyn_cast<ForInst>(inst)) {
            if (hasUnsafeStore(fi2->getBodyRegion(), addr, allowed)) return true;
        }
    }
    return false;
}

static StoreInst* findCanonicalIVUpdate(BasicBlock* bodyBB, Value* ivAddr, int& stepOut) {
    StoreInst* candidate = nullptr;
    for (auto* inst : bodyBB->getInstructions()) {
        if (auto* st = dyn_cast<StoreInst>(inst))
            if (st->getOperand(1) == ivAddr)
                candidate = st;
    }
    if (!candidate) return nullptr;

    auto* val = dyn_cast<BinaryInst>(candidate->getOperand(0));
    if (!val) return nullptr;

    auto opID = val->getOpID();
    Value* lhs = val->getOperand(0);
    Value* rhs = val->getOperand(1);

    auto isIVLoad = [&](Value* v) -> bool {
        auto* ld = dyn_cast<LoadInst>(v);
        return ld && ld->getOperand(0) == ivAddr;
    };

    if (opID == Instruction::Add) {
        if (isIVLoad(lhs) && isa<ConstantInt>(rhs)) {
            stepOut = cast<ConstantInt>(rhs)->getValue(); return candidate;
        }
        if (isIVLoad(rhs) && isa<ConstantInt>(lhs)) {
            stepOut = cast<ConstantInt>(lhs)->getValue(); return candidate;
        }
    }
    if (opID == Instruction::Sub && isIVLoad(lhs) && isa<ConstantInt>(rhs)) {
        stepOut = -cast<ConstantInt>(rhs)->getValue(); return candidate;
    }
    return nullptr;
}

struct CanonicalInfo {
    Value* ivAddr = nullptr;
    Value* bound = nullptr; // RHS of normalized cmp: load(ivAddr) pred bound
    ICmpInst::CmpOp pred = ICmpInst::SLT;
    int step = 0;
    StoreInst* ivStore = nullptr; // the canonical IV update store (end of body)
    Value* startVal = nullptr; // the initial value stored to ivAddr before the while
    std::vector<StoreInst*> preContinueStores; // IV stores immediately before each continue
};

static bool detect(WhileInst* wi, BasicBlock* parentBB, CanonicalInfo& out) {
    if (wi->getNumResults() != 0) return false;
    if (wi->getCondRegion()->getBlocks().size() != 1) return false;
    if (wi->getBodyRegion()->getBlocks().size() != 1) return false;

    auto* condBB = wi->getCondRegion()->getEntryBlock();
    auto* bodyBB = wi->getBodyRegion()->getEntryBlock();

    // condBB: the LAST instruction must be the ICmpInst that forms the loop condition.
    ICmpInst* loopCmp = nullptr;
    {
        Instruction* last = nullptr;
        for (auto* inst : condBB->getInstructions())
            last = inst;
        if (last) loopCmp = dyn_cast<ICmpInst>(last);
    }
    if (!loopCmp || !legalICmp(loopCmp)) return false;

    // Reject breaks (early exit cannot be represented in ForInst).
    if (hasBreak(wi->getBodyRegion())) return false;

    // Try each side of loopCmp as the IV.
    for (int ivSide : {0, 1}) {
        auto* ivCandidate = loopCmp->getOperand(ivSide);
        auto* ld = dyn_cast<LoadInst>(ivCandidate);
        if (!ld) continue;
        auto* addr = ld->getOperand(0);
        if (!isa<AllocaInst>(addr)) continue;

        int step = 0;
        StoreInst* ivStore = findCanonicalIVUpdate(bodyBB, addr, step);
        if (!ivStore || step == 0) continue;

        // Exactly one direct-level store to addr in bodyBB (the canonical update).
        int directStores = 0;
        for (auto* inst : bodyBB->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst))
                if (st->getOperand(1) == addr) ++directStores;
        }
        if (directStores != 1) continue;

        // Allow continues only if every ContinueInst is immediately preceded by a canonical IV store with the same step. 
        // Collect those stores for later removal.
        std::vector<ContinueInst*> conts;
        collectContinues(wi->getBodyRegion(), conts);
        std::vector<StoreInst*> preContinueStores;
        bool continuesOk = true;
        for (auto* cont : conts) {
            auto* contBB = cont->getParent();
            auto& instList = contBB->getInstructions();
            auto it = std::find(instList.begin(), instList.end(), static_cast<Instruction*>(cont));
            if (it == instList.begin()) { continuesOk = false; break; }
            auto* prevInst = *std::prev(it);
            if (!isIVRec(prevInst, addr, step)) { continuesOk = false; break; }
            preContinueStores.push_back(cast<StoreInst>(prevInst));
        }
        if (!continuesOk) continue;

        // No unaccounted nested stores to addr (only pre-continue stores are allowed).
        {
            std::set<Instruction*> allowed(preContinueStores.begin(), preContinueStores.end());
            if (hasUnsafeNestedStore(bodyBB, addr, allowed)) continue;
        }

        // Normalize pred so that it's: pred(load(ivAddr), bound).
        auto p = loopCmp->getPredicate();
        if (ivSide == 1) {
            switch (p) {
                case ICmpInst::SLT: p = ICmpInst::SGT; break;
                case ICmpInst::SLE: p = ICmpInst::SGE; break;
                case ICmpInst::SGT: p = ICmpInst::SLT; break;
                case ICmpInst::SGE: p = ICmpInst::SLE; break;
                default: break;
            }
        }

        // Step direction must match pred: SLT/SLE → step>0, SGT/SGE → step<0.
        bool increasing = (p == ICmpInst::SLT || p == ICmpInst::SLE);
        if (increasing && step <= 0) continue;
        if (!increasing && step >= 0) continue;

        // bound must not be stored inside body.
        auto* otherSide = loopCmp->getOperand(1 - ivSide);
        if (auto* ldBound = dyn_cast<LoadInst>(otherSide)) {
            if (mayWrite(wi->getCondRegion(), ldBound->getOperand(0)) ||
                mayWrite(wi->getBodyRegion(), ldBound->getOperand(0)))
                continue;
        }

        // Find start. For increasing loops, calls are only barriers when they
        // can write the candidate IV address. Keep descending loops conservative
        // because they exercise a different lowering path.
        Value* startVal = nullptr;
        auto& parentInsts = parentBB->getInstructions();
        auto wiPos = std::find(parentInsts.begin(), parentInsts.end(), static_cast<Instruction*>(wi));
        {
            auto it = wiPos;
            bool barrier = false;
            while (it != parentInsts.begin()) {
                --it;
                Instruction* cur = *it;
                if (isa<WhileInst>(cur) || isa<ForInst>(cur)) {
                    barrier = true; break;
                }
                if (isa<CallInst>(cur)) {
                    if (!increasing || mayWrite(cur, addr)) {
                        barrier = true; break;
                    }
                    continue;
                }
                if (isa<IfInst>(cur)) {
                    barrier = true; break;
                }
                if (auto* st = dyn_cast<StoreInst>(cur)) {
                    if (st->getOperand(1) == addr) {
                        startVal = st->getOperand(0);
                        break;
                    }
                }
            }
            if (barrier) startVal = nullptr;
        }
        if (!startVal) continue;

        out.ivAddr = addr;
        out.bound = otherSide;
        out.pred = p;
        out.step = step;
        out.ivStore = ivStore;
        out.startVal = startVal;
        out.preContinueStores = std::move(preContinueStores);
        return true;
    }
    return false;
}

}

bool WhileToFor::run() {
    bool any = false;
    for (auto* f : M->getFunctions()) any |= runFunc(f);
    return any;
}

bool WhileToFor::runFunc(Function* f) {
    if (!f || f->getBody()->getBlocks().empty()) return false;
    return runRegion(f->getBody());
}

bool WhileToFor::runRegion(Region* region) {
    bool changed = false;
    bool localChanged;
    do {
        localChanged = false;
        for (auto* bb : region->getBlocks()) {
            std::vector<Instruction*> snap(bb->getInstructions().begin(),
                                           bb->getInstructions().end());
            for (auto* inst : snap) {
                if (auto* wi = dyn_cast<WhileInst>(inst)) {
                    localChanged |= runRegion(wi->getCondRegion());
                    localChanged |= runRegion(wi->getBodyRegion());
                    if (whileImpl(wi)) { localChanged = true; break; }
                } else if (auto* ii = dyn_cast<IfInst>(inst)) {
                    localChanged |= runRegion(ii->getThenRegion());
                    if (ii->getElseRegion())
                        localChanged |= runRegion(ii->getElseRegion());
                }
            }
            if (localChanged) break;
        }
        changed |= localChanged;
    } while (localChanged);
    return changed;
}

bool WhileToFor::whileImpl(WhileInst* wi) {
    auto* parentBB = wi->getParent();
    if (!parentBB) return false;

    CanonicalInfo info;
    if (!detect(wi, parentBB, info)) return false;

    auto& insts = parentBB->getInstructions();
    auto pos = std::find(insts.begin(), insts.end(), static_cast<Instruction*>(wi));
    if (pos == insts.end()) return false;

    // Move any instructions from condBB that compute info.bound into parentBB before the ForInst. 
    // Without this, erasing the WhileInst would destroy the defs while ForInst still references them as its stop value.
    {
        auto* condBB = wi->getCondRegion()->getEntryBlock();
        std::vector<Instruction*> boundInsts;
        std::set<Value*> visited;

        std::function<void(Value*)> collect = [&](Value* v) {
            if (!v || !visited.insert(v).second) return;
            auto* inst = dyn_cast<Instruction>(v);
            if (!inst || inst->getParent() != condBB) return;
            for (unsigned i = 0; i < inst->getNumOperands(); ++i)
                collect(inst->getOperand(i));
            boundInsts.push_back(inst);
        };
        collect(info.bound);

        for (auto* inst : boundInsts) {
            inst->setParent(parentBB);
            insts.insert(pos, inst);
        }
    }

    // Build step constant.
    auto* stepConst = new ConstantInt(info.step);

    // Create ForInst.
    auto* fi = new ForInst(info.startVal, info.bound, stepConst,
                           info.ivAddr, info.pred, nullptr);
    fi->setName(wi->getName().empty() ? "for" : wi->getName() + ".for");
    fi->setParent(parentBB);

    // Move body instructions (except ivStore) from wi's bodyBB to fi's bodyBB.
    auto* wiBB = wi->getBodyRegion()->getEntryBlock();
    auto* fiBB = new BasicBlock("for_body", fi->getBodyRegion());

    for (auto* inst : wiBB->getInstructions()) {
        if (inst == info.ivStore) {
            inst->dropAllOperands();
            continue;
        }
        inst->setParent(fiBB);
        fiBB->getInstructions().push_back(inst);
    }
    wiBB->getInstructions().clear();

    // Erase pre-continue IV stores (now in nested IfInst regions within fiBB).
    // ForInst handles the increment implicitly; these explicit stores would double-increment.
    for (auto* st : info.preContinueStores)
        IRRewriter::eraseOp(st);

    insts.insert(pos, fi);
    wi->replaceAllUsesWith(nullptr);
    wi->eraseInst();

    return true;
}
