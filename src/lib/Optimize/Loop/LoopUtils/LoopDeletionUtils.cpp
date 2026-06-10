#include "../../../../include/Optimize/Loop/LoopUtils/LoopDeletionUtils.h"
#include <cstdint>

using namespace sysy;

static int DeadLoopElimID = 0;

// Give materialized instructions stable temporary names.
static void assignDLEName(Instruction* inst, const std::string& seed = "") {
    if (!inst || inst->getType()->isVoid()) return;
    if (!seed.empty())
        inst->setName(seed + ".dle" + std::to_string(DeadLoopElimID++));
    else
        inst->setName("%dle" + std::to_string(DeadLoopElimID++));
}

static Value* getBaseObject(Value* v, std::set<Value*>& vis) {
    if (!v || !vis.insert(v).second) return v;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return getBaseObject(gep->getOperand(0), vis);
    return v;
}

static Value* getBaseObject(Value* v) {
    std::set<Value*> vis;
    return getBaseObject(v, vis);
}

static bool isDeletionMaterializableImpl(Value* v, Loop* L, std::set<Value*>& vis) {
    if (!v || !L) return false;
    if (isa<Constant>(v) || isa<Argument>(v) || isa<GlobalVariable>(v) ||
        isa<Function>(v) || isa<BasicBlock>(v)) {
        return true;
    }

    auto* inst = dyn_cast<Instruction>(v);
    if (!inst || !inst->getParent()) return false;
    if (!L->has(inst->getParent())) return true;
    if (!vis.insert(v).second) return false;

    if (isa<LoadInst>(inst)) {
        auto* ptr = inst->getOperand(0);
        if (!isDeletionMaterializableImpl(ptr, L, vis))
            return false;
        Value* base = getBaseObject(ptr);
        return isa<GlobalVariable>(base) || isa<AllocaInst>(base) || isa<Argument>(base);
    }

    if (!(isa<BinaryInst>(inst) || isa<ICmpInst>(inst) || isa<FCmpInst>(inst) ||
          isa<CastInst>(inst) || isa<GetElementPtrInst>(inst))) {
        return false;
    }

    for (int i = 0; i < inst->getNumOperands(); ++i) {
        if (!isDeletionMaterializableImpl(inst->getOperand(i), L, vis))
            return false;
    }
    return true;
}

bool sysy::isDeletionMaterializable(Value* v, Loop* L) {
    std::set<Value*> vis;
    return isDeletionMaterializableImpl(v, L, vis);
}

static Instruction* cloneDeletionInst(
    Instruction* inst, const std::unordered_map<Value*, Value*>& vmap) {
    auto remap = [&](Value* v) -> Value* {
        auto it = vmap.find(v);
        return it != vmap.end() ? it->second : v;
    };

    Instruction* clone = nullptr;
    auto op = inst->getOpID();
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
    else if (isa<LoadInst>(inst))
        clone = new LoadInst(remap(inst->getOperand(0)), nullptr);
    else if (isa<GetElementPtrInst>(inst))
        clone = new GetElementPtrInst(remap(inst->getOperand(0)),
                                      remap(inst->getOperand(1)), nullptr);

    if (clone)
        assignDLEName(clone, inst->getName());
    return clone;
}

Value* sysy::materializeForDeletion(
    Value* v, Loop* L, BasicBlock* insertBB,
    std::unordered_map<Value*, Value*>& cache, std::set<Value*>& vis) {
    if (!v || !L || !insertBB) return nullptr;
    if (isa<Constant>(v) || isa<Argument>(v) || isa<GlobalVariable>(v) ||
        isa<Function>(v) || isa<BasicBlock>(v)) {
        return v;
    }

    auto* inst = dyn_cast<Instruction>(v);
    if (!inst || !inst->getParent()) return nullptr;
    if (!L->has(inst->getParent())) return v;

    auto it = cache.find(v);
    if (it != cache.end()) return it->second;
    if (!vis.insert(v).second) return nullptr;

    if (!(isa<BinaryInst>(inst) || isa<ICmpInst>(inst) || isa<FCmpInst>(inst) ||
          isa<CastInst>(inst) || isa<GetElementPtrInst>(inst))) {
        return nullptr;
    }

    std::unordered_map<Value*, Value*> vmap;
    for (int i = 0; i < inst->getNumOperands(); ++i) {
        Value* orig = inst->getOperand(i);
        Value* mat = materializeForDeletion(orig, L, insertBB, cache, vis);
        if (!mat) return nullptr;
        vmap[orig] = mat;
    }

    auto* clone = cloneDeletionInst(inst, vmap);
    if (!clone) return nullptr;
    clone->setParent(insertBB);
    auto& insts = insertBB->getInstructions();
    insts.insert(std::prev(insts.end()), clone);
    cache[v] = clone;
    return clone;
}

static int evalIcmp(ICmpInst::CmpOp pred, int64_t lhs, int64_t rhs) {
    switch (pred) {
        case ICmpInst::EQ: return lhs == rhs;
        case ICmpInst::NE: return lhs != rhs;
        case ICmpInst::SGT: return lhs >  rhs;
        case ICmpInst::SGE: return lhs >= rhs;
        case ICmpInst::SLT: return lhs <  rhs;
        case ICmpInst::SLE: return lhs <= rhs;
        default: return -1;
    }
}

// Own temporary constant objects used during local evaluation.
static ConstantInt* makeTempInt(int64_t v,
                                std::vector<std::unique_ptr<ConstantInt>>& owner) {
    owner.push_back(std::make_unique<ConstantInt>(v));
    return owner.back().get();
}

static ConstantInt* evaluateStaticInt(
    Value* v, std::vector<std::unique_ptr<ConstantInt>>& owner,
    std::unordered_map<Value*, ConstantInt*>& cache, std::set<Value*>& vis) {
    if (!v) return nullptr;
    if (auto* ci = dyn_cast<ConstantInt>(v)) return ci;
    if (isa<ConstantZero>(v)) return makeTempInt(0, owner);

    auto it = cache.find(v);
    if (it != cache.end()) return it->second;
    if (!vis.insert(v).second) return nullptr;

    if (auto* load = dyn_cast<LoadInst>(v)) {
        auto* gv = dyn_cast<GlobalVariable>(getBaseObject(load->getOperand(0)));
        if (!gv || !gv->isConst()) return nullptr;
        if (auto* ci = dyn_cast<ConstantInt>(gv->getInit())) {
            cache[v] = makeTempInt(ci->getValue(), owner);
            return cache[v];
        }
        if (isa<ConstantZero>(gv->getInit())) {
            cache[v] = makeTempInt(0, owner);
            return cache[v];
        }
        return nullptr;
    }

    if (auto* bin = dyn_cast<BinaryInst>(v)) {
        auto* lhs = evaluateStaticInt(bin->getOperand(0), owner, cache, vis);
        auto* rhs = evaluateStaticInt(bin->getOperand(1), owner, cache, vis);
        if (!lhs || !rhs) return nullptr;
        int64_t lv = lhs->getValue(), rv = rhs->getValue(), out = 0;
        switch (bin->getOpID()) {
            case Instruction::Add: out = lv + rv; break;
            case Instruction::Sub: out = lv - rv; break;
            case Instruction::Mul: out = lv * rv; break;
            case Instruction::Div: if (rv == 0) return nullptr; out = lv / rv; break;
            case Instruction::Mod: if (rv == 0) return nullptr; out = lv % rv; break;
            default: return nullptr;
        }
        cache[v] = makeTempInt(out, owner);
        return cache[v];
    }

    if (auto* cmp = dyn_cast<ICmpInst>(v)) {
        auto* lhs = evaluateStaticInt(cmp->getOperand(0), owner, cache, vis);
        auto* rhs = evaluateStaticInt(cmp->getOperand(1), owner, cache, vis);
        if (!lhs || !rhs) return nullptr;
        int r = evalIcmp(cmp->getPredicate(), lhs->getValue(), rhs->getValue());
        if (r < 0) return nullptr;
        cache[v] = makeTempInt(r, owner);
        return cache[v];
    }

    if (auto* cast = dyn_cast<CastInst>(v)) {
        auto* src = evaluateStaticInt(cast->getOperand(0), owner, cache, vis);
        if (!src) return nullptr;
        cache[v] = makeTempInt(src->getValue(), owner);
        return cache[v];
    }

    return nullptr;
}

// return 1 if cond is true, 0 if false, -1 if unknown
int sysy::evaluateDeletionCond(Value* cond, SCEV& scev) {
    if (auto* ci = dyn_cast<ConstantInt>(cond))
        return ci->getValue() != 0;

    {
        std::vector<std::unique_ptr<ConstantInt>> owner;
        std::unordered_map<Value*, ConstantInt*> cache;
        std::set<Value*> vis;
        if (auto* c = evaluateStaticInt(cond, owner, cache, vis))
            return c->getValue() != 0;
    }

    auto* cmp = dyn_cast<ICmpInst>(cond);
    if (!cmp) return -1;

    auto* lhs = dyn_cast<SEConst>(scev.get(cmp->getOperand(0)));
    auto* rhs = dyn_cast<SEConst>(scev.get(cmp->getOperand(1)));
    if (!lhs || !rhs) return -1;
    return evalIcmp(cmp->getPredicate(), lhs->val, rhs->val);
}

Value* sysy::evaluateFirstIterValue(
    Value* v, Loop* L, BasicBlock* entryPred, Dominators& dt,
    std::vector<std::unique_ptr<ConstantInt>>& tempOwner,
    std::unordered_map<Value*, Value*>& cache, std::set<Value*>& vis) {
    if (!v || !L || !entryPred) return nullptr;
    if (isa<Constant>(v) || isa<Argument>(v) || isa<GlobalVariable>(v) ||
        isa<Function>(v) || isa<BasicBlock>(v)) {
        return v;
    }

    auto it = cache.find(v);
    if (it != cache.end()) return it->second;

    auto* inst = dyn_cast<Instruction>(v);
    if (!inst || !inst->getParent()) return nullptr;
    if (!L->has(inst->getParent())) {
        std::unordered_map<Value*, ConstantInt*> ccache;
        std::set<Value*> cvis;
        if (auto* c = evaluateStaticInt(v, tempOwner, ccache, cvis)) {
            cache[v] = c;
            return c;
        }
        cache[v] = v;
        return v;
    }
    if (!vis.insert(v).second) return nullptr;

    if (auto* phi = dyn_cast<PhiInst>(inst)) {
        if (phi->getParent() != L->head) return nullptr;
        for (int k = 0; k < (int)phi->getNumOperands(); k += 2) {
            auto* fromBB = dyn_cast<BasicBlock>(phi->getOperand(k + 1));
            if (fromBB == entryPred) {
                Value* init = evaluateFirstIterValue(
                    phi->getOperand(k), L, entryPred, dt, tempOwner, cache, vis);
                if (init) cache[v] = init;
                return init;
            }
        }
        return nullptr;
    }

    if (inst->getParent() != L->latch && !dt.dominates(inst->getParent(), L->latch))
        return nullptr;

    if (isa<LoadInst>(inst)) {
        std::unordered_map<Value*, ConstantInt*> ccache;
        std::set<Value*> cvis;
        if (auto* c = evaluateStaticInt(v, tempOwner, ccache, cvis)) {
            cache[v] = c;
            return c;
        }
        return nullptr;
    }

    if (auto* bin = dyn_cast<BinaryInst>(inst)) {
        auto* lhs = dyn_cast<ConstantInt>(evaluateFirstIterValue(
            bin->getOperand(0), L, entryPred, dt, tempOwner, cache, vis));
        auto* rhs = dyn_cast<ConstantInt>(evaluateFirstIterValue(
            bin->getOperand(1), L, entryPred, dt, tempOwner, cache, vis));
        if (!lhs || !rhs) return nullptr;
        int64_t lv = lhs->getValue(), rv = rhs->getValue(), out = 0;
        switch (bin->getOpID()) {
            case Instruction::Add: out = lv + rv; break;
            case Instruction::Sub: out = lv - rv; break;
            case Instruction::Mul: out = lv * rv; break;
            case Instruction::Div: if (rv == 0) return nullptr; out = lv / rv; break;
            case Instruction::Mod: if (rv == 0) return nullptr; out = lv % rv; break;
            default: return nullptr;
        }
        auto* c = makeTempInt(out, tempOwner);
        cache[v] = c;
        return c;
    }

    if (auto* cmp = dyn_cast<ICmpInst>(inst)) {
        auto* lhs = dyn_cast<ConstantInt>(evaluateFirstIterValue(
            cmp->getOperand(0), L, entryPred, dt, tempOwner, cache, vis));
        auto* rhs = dyn_cast<ConstantInt>(evaluateFirstIterValue(
            cmp->getOperand(1), L, entryPred, dt, tempOwner, cache, vis));
        if (!lhs || !rhs) return nullptr;
        int r = evalIcmp(cmp->getPredicate(), lhs->getValue(), rhs->getValue());
        if (r < 0) return nullptr;
        auto* c = makeTempInt(r, tempOwner);
        cache[v] = c;
        return c;
    }

    return nullptr;
}
