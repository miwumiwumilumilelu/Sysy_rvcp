#include "../../../include/Optimize/High/InPlaceMatMulInterchange.h"
#include "../../../include/IR/IRRewriter.h"
#include <algorithm>
#include <set>
#include <vector>

using namespace sysy;

namespace {

struct Access {
    Value* root = nullptr;
    std::vector<Value*> indices;
};

static Value* uniqueStoredValue(AllocaInst* slot) {
    Value* result = nullptr;
    for (auto* user : slot->getUsers()) {
        if (auto* load = dyn_cast<LoadInst>(user))
            if (load->getOperand(0) == slot) continue;
        auto* store = dyn_cast<StoreInst>(user);
        if (!store || store->getOperand(1) != slot || result) return nullptr;
        result = store->getOperand(0);
    }
    return result;
}

static Value* stripSlotLoads(Value* value) {
    std::set<Value*> seen;
    while (value && seen.insert(value).second) {
        auto* load = dyn_cast<LoadInst>(value);
        auto* slot = load ? dyn_cast<AllocaInst>(load->getOperand(0)) : nullptr;
        if (!slot) break;
        Value* stored = uniqueStoredValue(slot);
        if (!stored) break;
        value = stored;
    }
    return value;
}

static Access decompose(Value* ptr) {
    Access result;
    std::vector<Value*> reversed;
    while (auto* gep = dyn_cast<GetElementPtrInst>(ptr)) {
        reversed.push_back(gep->getOperand(1));
        ptr = stripSlotLoads(gep->getOperand(0));
    }
    result.root = stripSlotLoads(ptr);
    result.indices.assign(reversed.rbegin(), reversed.rend());
    return result;
}

static bool isLoadOf(Value* value, Value* address) {
    auto* load = dyn_cast<LoadInst>(value);
    return load && load->getOperand(0) == address;
}

static bool sameScalar(Value* a, Value* b) {
    if (a == b) return true;
    auto* la = dyn_cast<LoadInst>(a);
    auto* lb = dyn_cast<LoadInst>(b);
    return la && lb && la->getOperand(0) == lb->getOperand(0);
}

static bool isZero(Value* value) {
    auto* c = dyn_cast<ConstantInt>(value);
    return c && c->getValue() == 0;
}

static bool isOne(Value* value) {
    auto* c = dyn_cast<ConstantInt>(value);
    return c && c->getValue() == 1;
}

static ForInst* parentFor(ForInst* loop) {
    if (!loop || !loop->getParent() || !loop->getParent()->getParent()) return nullptr;
    return dyn_cast<ForInst>(loop->getParent()->getParent()->getParentInst());
}

static void collect(Region* region, std::vector<Instruction*>& out) {
    if (!region) return;
    for (auto* bb : region->getBlocks())
        for (auto* inst : bb->getInstructions()) {
            out.push_back(inst);
            for (auto& nested : inst->getRegions()) collect(nested.get(), out);
        }
}

static bool isIndex(Value* value, ForInst* loop) {
    return loop && isLoadOf(value, loop->getIVAddr());
}

struct Match {
    ForInst* outer = nullptr;
    ForInst* middle = nullptr;
    ForInst* inner = nullptr;
    GlobalVariable* left = nullptr;
    GlobalVariable* right = nullptr;
    Value* sumAddress = nullptr;
};

static bool accessIs(const Access& access, Value* root, ForInst* d0,
                     ForInst* d1) {
    return access.root == root && access.indices.size() == 3 &&
           isZero(access.indices[0]) && isIndex(access.indices[1], d0) &&
           isIndex(access.indices[2], d1);
}

static bool matchTriple(ForInst* inner, Match& match) {
    ForInst* middle = parentFor(inner);
    ForInst* outer = parentFor(middle);
    if (!outer || !middle || !inner) return false;
    for (auto* loop : {outer, middle, inner})
        if (!isZero(loop->getStart()) || !isOne(loop->getStep()) ||
            loop->getPred() != ICmpInst::SLT)
            return false;
    if (!sameScalar(outer->getStop(), middle->getStop()) ||
        !sameScalar(outer->getStop(), inner->getStop()))
        return false;

    StoreInst* accumulation = nullptr;
    std::vector<Instruction*> innerInsts;
    collect(inner->getBodyRegion(), innerInsts);
    for (auto* inst : innerInsts) {
        auto* store = dyn_cast<StoreInst>(inst);
        if (!store) {
            if (isa<CallInst>(inst) || isa<IfInst>(inst) || isa<WhileInst>(inst) ||
                isa<ForInst>(inst) || isa<BreakInst>(inst) ||
                isa<ContinueInst>(inst))
                return false;
            continue;
        }
        auto* add = dyn_cast<BinaryInst>(store->getOperand(0));
        if (!add || add->getOpID() != Instruction::Add || accumulation)
            return false;
        LoadInst* sumLoad = nullptr;
        BinaryInst* product = nullptr;
        for (int order = 0; order < 2; ++order) {
            sumLoad = dyn_cast<LoadInst>(add->getOperand(order));
            product = dyn_cast<BinaryInst>(add->getOperand(1 - order));
            if (sumLoad && sumLoad->getOperand(0) == store->getOperand(1) &&
                product && product->getOpID() == Instruction::Mul)
                break;
            sumLoad = nullptr;
            product = nullptr;
        }
        if (!sumLoad || !product) return false;
        auto* lhsLoad = dyn_cast<LoadInst>(product->getOperand(0));
        auto* rhsLoad = dyn_cast<LoadInst>(product->getOperand(1));
        if (!lhsLoad || !rhsLoad) return false;
        Access lhs = decompose(lhsLoad->getOperand(0));
        Access rhs = decompose(rhsLoad->getOperand(0));
        auto* left = dyn_cast<GlobalVariable>(lhs.root);
        auto* right = dyn_cast<GlobalVariable>(rhs.root);
        if (!left || !right || left == right ||
            !accessIs(lhs, left, outer, inner) ||
            !accessIs(rhs, right, inner, middle))
            return false;
        match.left = left;
        match.right = right;
        match.sumAddress = store->getOperand(1);
        accumulation = store;
    }
    if (!accumulation) return false;

    StoreInst* resultStore = nullptr;
    std::vector<Instruction*> middleInsts;
    for (auto* bb : middle->getBodyRegion()->getBlocks())
        for (auto* inst : bb->getInstructions()) {
            if (inst == inner) continue;
            auto* store = dyn_cast<StoreInst>(inst);
            if (!store) continue;
            Access dst = decompose(store->getOperand(1));
            if (!accessIs(dst, match.right, outer, middle)) continue;
            if (!isLoadOf(store->getOperand(0), match.sumAddress) || resultStore)
                return false;
            resultStore = store;
        }
    if (!resultStore) return false;

    // Every matrix access in the candidate nest must be one of the three
    // proven accesses: left[i][k], right[k][j], or right[i][j].
    std::vector<Instruction*> all;
    collect(middle->getBodyRegion(), all);
    for (auto* inst : all) {
        if ((isa<ForInst>(inst) && inst != inner) || isa<IfInst>(inst) ||
            isa<WhileInst>(inst) || isa<CallInst>(inst) || isa<BreakInst>(inst) ||
            isa<ContinueInst>(inst) || isa<ReturnInst>(inst))
            return false;
        Value* ptr = nullptr;
        if (auto* load = dyn_cast<LoadInst>(inst)) ptr = load->getOperand(0);
        if (auto* store = dyn_cast<StoreInst>(inst)) ptr = store->getOperand(1);
        if (!ptr) continue;
        Access access = decompose(ptr);
        if (auto* store = dyn_cast<StoreInst>(inst)) {
            if (access.root != match.left && access.root != match.right &&
                ptr != match.sumAddress && ptr != inner->getIVAddr())
                return false;
        }
        if (isa<GlobalVariable>(access.root) && access.root != match.left &&
            access.root != match.right)
            return false;
        if (access.root != match.left && access.root != match.right) continue;
        bool allowed = accessIs(access, match.left, outer, inner) ||
                       accessIs(access, match.right, inner, middle) ||
                       accessIs(access, match.right, outer, middle);
        if (!allowed) return false;
    }
    match.outer = outer;
    match.middle = middle;
    match.inner = inner;
    return true;
}

static Value* appendElement(GlobalVariable* global, Value* row, Value* column,
                            BasicBlock* bb, const char* prefix) {
    auto* base = new GetElementPtrInst(global, new ConstantInt(0), bb);
    base->setName(std::string(prefix) + ".base");
    auto* rowPtr = new GetElementPtrInst(base, row, bb);
    rowPtr->setName(std::string(prefix) + ".row");
    auto* element = new GetElementPtrInst(rowPtr, column, bb);
    element->setName(std::string(prefix) + ".element");
    return element;
}

static ForInst* makeLoopLike(ForInst* original, BasicBlock* parent,
                             const char* name) {
    auto* loop = new ForInst(original->getStart(), original->getStop(),
                             original->getStep(), original->getIVAddr(),
                             original->getPred(), parent);
    loop->setName(name);
    return loop;
}

static void buildInitialization(const Match& m, ForInst* init) {
    auto* bb = new BasicBlock("ipmm.init.body", init->getBodyRegion());
    auto* i = new LoadInst(m.outer->getIVAddr(), bb); i->setName("ipmm.i");
    auto* j = new LoadInst(m.middle->getIVAddr(), bb); j->setName("ipmm.j");
    Value* diagonalPtr = appendElement(m.left, i, i, bb, "ipmm.diag");
    auto* diagonal = new LoadInst(diagonalPtr, bb); diagonal->setName("ipmm.diag.v");
    Value* oldPtr = appendElement(m.right, i, j, bb, "ipmm.old");
    auto* oldValue = new LoadInst(oldPtr, bb); oldValue->setName("ipmm.old.v");
    auto* product = new BinaryInst(Instruction::Mul, diagonal, oldValue, bb);
    product->setName("ipmm.diag.product");
    new StoreInst(product, oldPtr, bb);
}

static void buildAccumulation(const Match& m, ForInst* kLoop) {
    auto* kBB = new BasicBlock("ipmm.k.body", kLoop->getBodyRegion());
    auto* i = new LoadInst(m.outer->getIVAddr(), kBB); i->setName("ipmm.i");
    auto* k = new LoadInst(m.inner->getIVAddr(), kBB); k->setName("ipmm.k");
    auto* notDiagonal = new ICmpInst(ICmpInst::NE, k, i, kBB);
    notDiagonal->setName("ipmm.not.diagonal");
    auto* guard = new IfInst(notDiagonal, kBB);
    guard->setName("ipmm.guard");

    auto* thenBB = new BasicBlock("ipmm.guard.body", guard->getThenRegion());
    auto* jLoop = makeLoopLike(m.middle, thenBB, "ipmm.j.loop");
    auto* jBB = new BasicBlock("ipmm.j.body", jLoop->getBodyRegion());
    auto* ii = new LoadInst(m.outer->getIVAddr(), jBB); ii->setName("ipmm.i");
    auto* kk = new LoadInst(m.inner->getIVAddr(), jBB); kk->setName("ipmm.k");
    auto* j = new LoadInst(m.middle->getIVAddr(), jBB); j->setName("ipmm.j");
    Value* coeffPtr = appendElement(m.left, ii, kk, jBB, "ipmm.coeff");
    auto* coeff = new LoadInst(coeffPtr, jBB); coeff->setName("ipmm.coeff.v");
    Value* srcPtr = appendElement(m.right, kk, j, jBB, "ipmm.src");
    auto* src = new LoadInst(srcPtr, jBB); src->setName("ipmm.src.v");
    Value* dstPtr = appendElement(m.right, ii, j, jBB, "ipmm.dst");
    auto* dst = new LoadInst(dstPtr, jBB); dst->setName("ipmm.dst.v");
    auto* product = new BinaryInst(Instruction::Mul, coeff, src, jBB);
    product->setName("ipmm.product");
    auto* sum = new BinaryInst(Instruction::Add, dst, product, jBB);
    sum->setName("ipmm.sum");
    new StoreInst(sum, dstPtr, jBB);
}

static void transform(const Match& match) {
    BasicBlock* parent = match.middle->getParent();
    auto& instructions = parent->getInstructions();
    auto position = std::find(instructions.begin(), instructions.end(), match.middle);

    auto* init = makeLoopLike(match.middle, nullptr, "ipmm.init.loop");
    buildInitialization(match, init);
    init->setParent(parent);
    instructions.insert(position, init);

    auto* kLoop = makeLoopLike(match.inner, nullptr, "ipmm.k.loop");
    // The original k bound is materialized inside the j body and is deleted
    // with that body.  All three bounds were proven equal, so retain the
    // outer bound, whose definition dominates the replacement nest.
    kLoop->setOperand(1, match.outer->getStop());
    buildAccumulation(match, kLoop);
    kLoop->setParent(parent);
    instructions.insert(position, kLoop);

    IRRewriter::eraseOp(match.middle);
}

} // namespace

bool InPlaceMatMulInterchange::run() {
    for (auto* function : M->getFunctions()) {
        std::vector<Instruction*> instructions;
        collect(function->getBody(), instructions);
        for (auto* inst : instructions) {
            auto* inner = dyn_cast<ForInst>(inst);
            if (!inner) continue;
            Match match;
            if (!matchTriple(inner, match)) continue;
            transform(match);
            return true;
        }
    }
    return false;
}
