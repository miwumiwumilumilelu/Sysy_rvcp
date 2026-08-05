#include "../../../include/Optimize/Scalar/Memoize.h"
#include "../../../include/Optimize/Analysis/RecursiveAffine.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/LoopInfo.h"
#include "../../../include/Optimize/Scalar/SSAInline.h"
#include "../../../include/IR/Instruction.h"
#include "../../../include/IR/Value.h"
#include "../../../include/IR/Type.h"
#include <algorithm>
#include <limits>
#include <vector>

using namespace sysy;

// The runtime guard makes any choice safe
static const int limit = 1000;

// Keep each generated cache small enough to remain hot.  Capacity is selected
// per function from this byte budget because entries grow with the key count.
// Collisions replace old entries, but full-key comparison preserves correctness.
static const int hashCacheBytes = 80 * 1024;
// Projecting an accumulator exposes many more reusable states than exact-key
// memoization, while a modest table is still small enough to stay cache-local.
static const int affineHashCacheBytes = 4 * 1024 * 1024;
// Dense summaries trade zero-initialized address space for predictable O(1)
// lookup.  Only touched pages become resident, so a generous budget extends
// the fast range without increasing the working set of smaller inputs.
static const int affineDenseCacheBytes = 64 * 1024 * 1024;
static const int hashPathCapacity = 4096;

GlobalVariable* Memoize::traceToGlobal(Value* v) {
    while (v) {
        if (auto* g = dyn_cast<GlobalVariable>(v)) return g;
        if (auto* gep = dyn_cast<GetElementPtrInst>(v)) {
            v = gep->getOperand(0);
            continue;
        }
        return nullptr;
    }
    return nullptr;
}

bool Memoize::isCandidate2D(Function* f, int& iB) {
    if (f->getBody()->getBlocks().empty()) return false;

    // Exactly two i32 params, i32 return.
    // Design for 2D Memoize.
    auto& args = f->getArgs();
    if (args.size() != 2) return false;
    if (!args[0]->getType()->isInt() || !args[1]->getType()->isInt()) return false;
    if (!f->getType()->isInt()) return false;

    int calls = 0;
    int arrayBound = 0;
    for (auto* bb : f->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            // f must not write any memory (purity/ cache validity).
            if (isa<StoreInst>(inst)) return false;
            // f only call itself.
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (call->getFunction() != f) return false;
                calls++;
            }
            // Find the bound of i.
            if (auto* ld = dyn_cast<LoadInst>(inst)) {
                if (auto* g = traceToGlobal(ld->getOperand(0))) {
                    // Which is 2D gvArray.
                    // here arrayBound is the maxSize of first-dim of all gvArrays.
                    if (auto* pt = dyn_cast<PointerType>(g->getType())) {
                        if (auto* at = dyn_cast<ArrayType>(pt->getPointeeType())) {
                            int n = at->getNumElements();
                            if (n > arrayBound) arrayBound = n;
                        }
                    }
                }
            }
        }
    }

    // Check if it is a recursive function.
    if (calls == 0) return false;
    if (arrayBound == 0) return false;

    // f can only be entered once in main.
    int ext = 0;
    for (auto* g : M->getFunctions()) {
        if (g == f) continue;
        for (auto* bb : g->getBody()->getBlocks())
            for (auto* inst : bb->getInstructions())
                if (auto* call = dyn_cast<CallInst>(inst)) 
                    if (call->getFunction() == f) 
                        ext++;
    }
    if (ext != 1) return false;

    iB = arrayBound;
    return true;
}

bool Memoize::transform2D(Function* f, int iB) {
    // One common bound B for both dimensions.
    // f[i][w] and a deliberately swapped f[w][i] both work.
    const int B = (iB > limit ? iB : limit);
    const int stride = B + 1;
    const int size = stride * stride;

    Type* i32 = Type::getIntTy();
    Function* body = f;
    std::string origName = body->getName();
    body->setName(origName + "_memoFunc");

    Function* wrapper = new Function(origName, body->getType());
    auto* argI = new Argument(i32, "i", wrapper, 0);
    auto* argW = new Argument(i32, "w", wrapper, 1);
    wrapper->addArgument(argI);
    wrapper->addArgument(argW);
    M->addFunction(wrapper);

    auto* arrTy = new ArrayType(i32, size);
    auto* memoG = new GlobalVariable(origName + "_memo", arrTy, new ConstantZero(arrTy));
    auto* doneG = new GlobalVariable(origName + "_done", arrTy, new ConstantZero(arrTy));
    M->addGlobalVariable(memoG);
    M->addGlobalVariable(doneG);

    // Redirect call targeting the body.
    for (auto* f : M->getFunctions()) {
        if (f == wrapper) continue;
        for (auto* bb : f->getBody()->getBlocks()) 
            for (auto* inst : bb->getInstructions())
                if (auto* call = dyn_cast<CallInst>(inst))
                    if (call->getFunction() == body)
                        call->setOperand(0, wrapper);
    }

    // Build the wrapper's CFG.
    Region* region = wrapper->getBody();
    auto* entry = new BasicBlock("entry", region);
    auto* check = new BasicBlock("check", region);
    auto* hit = new BasicBlock("hit", region);
    auto* miss = new BasicBlock("miss", region);
    auto* slow = new BasicBlock("slow", region);

    auto C = [](int v) {
        return new ConstantInt(v);
    };

    // entry:
    //      inb = 1<=i<=B && 1<=w<=B;
    //      br inb, check, slow;
    auto* c0 = new ICmpInst(ICmpInst::SGE, argI, C(1), entry);
    c0->setName(nm());
    auto* c1 = new ICmpInst(ICmpInst::SLE, argI, C(B), entry);
    c1->setName(nm());
    auto* c2 = new ICmpInst(ICmpInst::SGE, argW, C(1), entry); 
    c2->setName(nm());
    auto* c3 = new ICmpInst(ICmpInst::SLE, argW, C(B), entry);
    c3->setName(nm());
    auto* a0 = new BinaryInst(Instruction::And, c0, c1, entry); 
    a0->setName(nm());
    auto* a1 = new BinaryInst(Instruction::And, c2, c3, entry); 
    a1->setName(nm());
    auto* inb = new BinaryInst(Instruction::And, a0, a1, entry); 
    inb->setName(nm());
    new BranchInst(inb, check, slow, entry);

    // check:
    //      idx = i*stride + w; 
    //      d = done[idx];  
    //      br (d != 0), hit, miss;
    auto* mul = new BinaryInst(Instruction::Mul, argI, C(stride), check);
    mul->setName(nm());
    auto* idx = new BinaryInst(Instruction::Add, mul, argW, check);
    idx->setName(nm());
    auto* dd0 = new GetElementPtrInst(doneG, C(0), check); 
    dd0->setName(nm());
    auto* ddp = new GetElementPtrInst(dd0, idx, check);    
    ddp->setName(nm());
    auto* d = new LoadInst(ddp, check);                 
    d->setName(nm());
    auto* hc = new ICmpInst(ICmpInst::NE, d, C(0), check); 
    hc->setName(nm());
    new BranchInst(hc, hit, miss, check);

    // hit: 
    //      return memo[idx];
    auto* mm0 = new GetElementPtrInst(memoG, C(0), hit); 
    mm0->setName(nm());
    auto* mmp = new GetElementPtrInst(mm0, idx, hit);   
    mmp->setName(nm());
    auto* m = new LoadInst(mmp, hit);                 
    m->setName(nm());
    new ReturnInst(m, hit);

    // miss: 
    //      r = body(i, w); 
    //      done[idx] = 1;  
    //      memo[idx] = r;  
    //      return r;
    auto* r = new CallInst(body, {argI, argW}, miss); 
    r->setName(nm());
    auto* sd0 = new GetElementPtrInst(doneG, C(0), miss); 
    sd0->setName(nm());
    auto* sdp = new GetElementPtrInst(sd0, idx, miss);    
    sdp->setName(nm());
    new StoreInst(C(1), sdp, miss);
    auto* sm0 = new GetElementPtrInst(memoG, C(0), miss); 
    sm0->setName(nm());
    auto* smp = new GetElementPtrInst(sm0, idx, miss);    
    smp->setName(nm());
    new StoreInst(r, smp, miss);
    new ReturnInst(r, miss);

    // slow: 
    //      run the original recursion, never touch the table.
    auto* r2 = new CallInst(body, {argI, argW}, slow); 
    r2->setName(nm());
    new ReturnInst(r2, slow);

    return true;
}

bool Memoize::isCandidate1D(Function* f, int& iB) {
    if (f->getBody()->getBlocks().empty()) return false;

    // Exactly one i32 param, i32 return.
    // Design for 1D Memoize.
    auto& args = f->getArgs();
    if (args.size() != 1) return false;
    if (!args[0]->getType()->isInt()) return false;
    if (!f->getType()->isInt()) return false;

    int calls = 0;
    int arrayBound = 0;
    for (auto* bb : f->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            // f must not write any memory (purity/ cache validity).
            if (isa<StoreInst>(inst)) return false;
            // f only call itself.
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (call->getFunction() != f) return false;
                calls++;
            }
            // Find the bound of i from the global arrays it indexes.
            if (auto* ld = dyn_cast<LoadInst>(inst)) {
                if (auto* g = traceToGlobal(ld->getOperand(0))) {
                    if (auto* pt = dyn_cast<PointerType>(g->getType())) {
                        if (auto* at = dyn_cast<ArrayType>(pt->getPointeeType())) {
                            int n = at->getNumElements();
                            if (n > arrayBound) arrayBound = n;
                        }
                    }
                }
            }
        }
    }

    // Need branching recursion (>=2 self-call sites) for overlapping subproblems;
    // a single recursive call is linear and gains nothing from memoization.
    if (calls < 2) return false;

    // f can only be entered once in main.
    int ext = 0;
    for (auto* g : M->getFunctions()) {
        if (g == f) continue;
        for (auto* bb : g->getBody()->getBlocks())
            for (auto* inst : bb->getInstructions())
                if (auto* call = dyn_cast<CallInst>(inst))
                    if (call->getFunction() == f)
                        ext++;
    }
    if (ext != 1) return false;

    // i bounded by the arrays it indexes, or the fixed bound if it indexes none
    iB = arrayBound ? arrayBound : limit;
    return true;
}

bool Memoize::transform1D(Function* f, int iB) {
    // f[i]
    const int size = iB + 1;

    Type* i32 = Type::getIntTy();
    Function* body = f;
    std::string origName = body->getName();
    body->setName(origName + "_memoFunc");

    Function* wrapper = new Function(origName, body->getType());
    auto* argI = new Argument(i32, "i", wrapper, 0);
    wrapper->addArgument(argI);
    M->addFunction(wrapper);

    auto* arrTy = new ArrayType(i32, size);
    auto* memoG = new GlobalVariable(origName + "_memo", arrTy, new ConstantZero(arrTy));
    auto* doneG = new GlobalVariable(origName + "_done", arrTy, new ConstantZero(arrTy));
    M->addGlobalVariable(memoG);
    M->addGlobalVariable(doneG);

    // Redirect call targeting the body.
    for (auto* g : M->getFunctions()) {
        if (g == wrapper) continue;
        for (auto* bb : g->getBody()->getBlocks())
            for (auto* inst : bb->getInstructions())
                if (auto* call = dyn_cast<CallInst>(inst))
                    if (call->getFunction() == body)
                        call->setOperand(0, wrapper);
    }

    // Build the wrapper's CFG.
    Region* region = wrapper->getBody();
    auto* entry = new BasicBlock("entry", region);
    auto* check = new BasicBlock("check", region);
    auto* hit = new BasicBlock("hit", region);
    auto* miss = new BasicBlock("miss", region);
    auto* slow = new BasicBlock("slow", region);

    auto C = [](int v) {
        return new ConstantInt(v);
    };

    // entry:
    //      inb = 1<=i<=iB;
    //      br inb, check, slow;
    auto* c0 = new ICmpInst(ICmpInst::SGE, argI, C(1), entry);
    c0->setName(nm());
    auto* c1 = new ICmpInst(ICmpInst::SLE, argI, C(iB), entry);
    c1->setName(nm());
    auto* inb = new BinaryInst(Instruction::And, c0, c1, entry);
    inb->setName(nm());
    new BranchInst(inb, check, slow, entry);

    // check:
    //      idx = i;
    //      d = done[idx];
    //      br (d != 0), hit, miss;
    Value* idx = argI;
    auto* dd0 = new GetElementPtrInst(doneG, C(0), check);
    dd0->setName(nm());
    auto* ddp = new GetElementPtrInst(dd0, idx, check);
    ddp->setName(nm());
    auto* d = new LoadInst(ddp, check);
    d->setName(nm());
    auto* hc = new ICmpInst(ICmpInst::NE, d, C(0), check);
    hc->setName(nm());
    new BranchInst(hc, hit, miss, check);

    // hit:
    //      return memo[idx];
    auto* mm0 = new GetElementPtrInst(memoG, C(0), hit);
    mm0->setName(nm());
    auto* mmp = new GetElementPtrInst(mm0, idx, hit);
    mmp->setName(nm());
    auto* m = new LoadInst(mmp, hit);
    m->setName(nm());
    new ReturnInst(m, hit);

    // miss:
    //      r = body(i);
    //      done[idx] = 1;
    //      memo[idx] = r;
    //      return r;
    auto* r = new CallInst(body, {argI}, miss);
    r->setName(nm());
    auto* sd0 = new GetElementPtrInst(doneG, C(0), miss);
    sd0->setName(nm());
    auto* sdp = new GetElementPtrInst(sd0, idx, miss);
    sdp->setName(nm());
    new StoreInst(C(1), sdp, miss);
    auto* sm0 = new GetElementPtrInst(memoG, C(0), miss);
    sm0->setName(nm());
    auto* smp = new GetElementPtrInst(sm0, idx, miss);
    smp->setName(nm());
    new StoreInst(r, smp, miss);
    new ReturnInst(r, miss);

    // slow:
    //      run the original recursion, never touch the table.
    auto* r2 = new CallInst(body, {argI}, slow);
    r2->setName(nm());
    new ReturnInst(r2, slow);

    return true;
}

bool Memoize::isHashCandidate(
    Function* f, std::vector<GlobalVariable*>& scalarGlobals) {
    if (f->getBody()->getBlocks().empty()) return false;

    // Only support i32.
    auto& args = f->getArgs();
    if (args.empty() || !f->getType()->isInt()) return false;
    for (auto* arg : args)
        if (!arg->getType()->isInt()) return false;

    int recursiveCalls = 0;
    scalarGlobals.clear();

    for (auto* bb : f->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            // store or unknown call could change state 
            // while a cached result is being computed.
            if (isa<StoreInst>(inst)) return false;
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (call->getFunction() != f) return false;
                recursiveCalls++;
            }

            // Only support load i32 gv.
            if (auto* load = dyn_cast<LoadInst>(inst)) {
                auto* gv = dyn_cast<GlobalVariable>(load->getOperand(0));
                if (!gv) return false;

                auto* ptrTy = dyn_cast<PointerType>(gv->getType());
                if (!ptrTy || !ptrTy->getPointeeType()->isInt()) return false;

                // Constants do not need key space.  
                // A mutable scalar global is
                // loaded by the wrapper and compared as part of the full key.
                if (!gv->isConst() &&
                    std::find(scalarGlobals.begin(), scalarGlobals.end(), gv) == scalarGlobals.end())
                    scalarGlobals.push_back(gv);
            }
        }
    }

    // Linear recursion normally has no overlapping subproblems and would only
    // pay cache overhead.  Multiple recursive sites are a general, conservative
    // profitability filter rather than a correctness requirement.
    return recursiveCalls >= 2;
}

bool Memoize::isAllTailRecursive(Function* f) {
    bool found = false;
    for (auto* bb : f->getBody()->getBlocks()) {
        auto& insts = bb->getInstructions();
        for (auto it = insts.begin(); it != insts.end(); ++it) {
            auto* call = dyn_cast<CallInst>(*it);
            if (!call || call->getFunction() != f) continue;
            found = true;
            auto next = std::next(it);
            if (next == insts.end()) return false;
            auto* ret = dyn_cast<ReturnInst>(*next);
            if (!ret || ret->getNumOperands() != 1 || ret->getOperand(0) != call)
                return false;
        }
    }
    return found;
}

bool Memoize::isStableScalarGlobal(Function* f, GlobalVariable* global) {
    // The address must not escape through a GEP/call/phi,
    // with only direct loads and stores.
    StoreInst* uniqueStore = nullptr;
    for (auto* user : global->getUsers()) {
        if (isa<LoadInst>(user)) continue;
        auto* store = dyn_cast<StoreInst>(user);
        // Unique initialization of gv.
        if (!store || store->getOperand(1) != global) return false;
        if (uniqueStore && uniqueStore != store) return false;
        uniqueStore = store;
    }

    // No store means the uStoreFunc is the value for the whole program.
    if (!uniqueStore) return true;

    Function* uStoreFunc = uniqueStore->getParent()->getParentFunc();
    if (!uStoreFunc || uStoreFunc->getName() != "main") return false;

    Dominators dt(uStoreFunc);
    dt.run();
    LoopInfo li(uStoreFunc, dt);

    // Reject uStore in loop.
    if (li.loopOf(uniqueStore->getParent())) 
        return false;

    bool hasf = false;
    for (auto* caller : M->getFunctions()) {
        if (caller == f) continue;
        for (auto* bb : caller->getBody()->getBlocks()) {
            for (auto* inst : bb->getInstructions()) {
                auto* call = dyn_cast<CallInst>(inst);
                if (!call || call->getFunction() != f) continue;
                hasf = true;
                // uStoreFunc is gv's parentFunc.
                if (caller != uStoreFunc ||
                    !dt.dominates(uniqueStore->getParent(), bb))
                    return false;
                if (bb == uniqueStore->getParent()) {
                    auto& instructions = bb->getInstructions();
                    auto storeIt = std::find(instructions.begin(),
                                             instructions.end(), uniqueStore);
                    auto callIt =
                        std::find(instructions.begin(), instructions.end(), call);
                    if (storeIt == instructions.end() ||
                        callIt == instructions.end() ||
                        // storeIt > callIt
                        std::distance(storeIt, callIt) <= 0)
                        return false;
                }
            }
        }
    }
    return hasf;
}

void Memoize::instrumentAffineCoefficient(
    Function* body, const RecursiveAffineSummary& summary,
    GlobalVariable* coefficientGlobal) {
    auto C = [](uint32_t value) {
        return new ConstantInt(static_cast<int32_t>(value));
    };
    auto moveLastBefore = [](BasicBlock* bb,
                             std::list<Instruction*>::iterator position) {
        auto& instructions = bb->getInstructions();
        instructions.splice(position, instructions,
                            std::prev(instructions.end()));
    };

    // A terminal c*x+d publishes c.  The wrapper obtains d by invoking the
    // body with x=0.
    for (const auto& terminal : summary.terminals) {
        auto* bb = terminal.ret->getParent();
        auto& instructions = bb->getInstructions();
        auto position = std::find(instructions.begin(), instructions.end(),
                                  terminal.ret);
        auto* store = new StoreInst(C(terminal.coefficient), coefficientGlobal,
                                    bb);
        (void)store;
        moveLastBefore(bb, position);
    }

    // A child summary A*(a*x+b)+B has coefficient A*a with respect to the
    // parent's accumulator.  Strict tail recursion lets one scratch scalar
    // carry that coefficient without a pair return type.
    for (const auto& transition : summary.transitions) {
        if (transition.coefficient == 1) continue;
        auto* bb = transition.call->getParent();
        auto& instructions = bb->getInstructions();
        auto callPosition = std::find(instructions.begin(), instructions.end(),
                                      transition.call);
        auto position = std::next(callPosition);
        if (transition.coefficient == 0) {
            new StoreInst(C(0), coefficientGlobal, bb);
            moveLastBefore(bb, position);
            continue;
        }
        auto* childCoefficient = new LoadInst(coefficientGlobal, bb);
        childCoefficient->setName(nm());
        moveLastBefore(bb, position);
        auto* composed = new BinaryInst(Instruction::Mul, childCoefficient,
                                        C(transition.coefficient), bb);
        composed->setName(nm());
        moveLastBefore(bb, position);
        new StoreInst(composed, coefficientGlobal, bb);
        moveLastBefore(bb, position);
    }
}

bool Memoize::transformAffineDense(
    Function* f, const std::vector<GlobalVariable*>& scalarGlobals,
    const RecursiveAffineSummary& summary) {
    const unsigned accumulatorIndex = summary.accumulatorIndex;
    auto& bodyArguments = f->getArgs();
    if (bodyArguments.size() != 2 || accumulatorIndex >= bodyArguments.size())
        return false;

    const unsigned keyIndex = accumulatorIndex == 0 ? 1 : 0;
    const bool compactCoefficient = summary.booleanCoefficient;
    // General layout: [valid, A, B, globals...].  When A is proven boolean,
    // encode valid and A together as tag=A+1: [tag, B, globals...].
    const unsigned valueWords = compactCoefficient ? 2u : 3u;
    const unsigned biasOffset = compactCoefficient ? 1u : 2u;
    const unsigned entryWords = valueWords + scalarGlobals.size();
    const int entryBytes = static_cast<int>(entryWords) * 4;
    const int maxEntries = affineDenseCacheBytes / entryBytes;
    if (maxEntries <= 0) return false;
    int capacity = 1;
    while (capacity <= maxEntries / 2) capacity <<= 1;

    Type* i32 = Type::getIntTy();
    Function* body = f;
    std::string origName = body->getName();
    body->setName(origName + "_memoFunc");

    Function* wrapper = new Function(origName, body->getType());
    std::vector<Value*> wrapperArgs;
    wrapperArgs.reserve(bodyArguments.size());
    for (unsigned i = 0; i < bodyArguments.size(); ++i) {
        auto* oldArg = bodyArguments[i];
        auto* arg = new Argument(oldArg->getType(), oldArg->getName(), wrapper, i);
        wrapper->addArgument(arg);
        wrapperArgs.push_back(arg);
    }
    M->addFunction(wrapper);

    const int tableSize = capacity * static_cast<int>(entryWords);
    auto* tableTy = new ArrayType(i32, tableSize);
    auto* tableG = new GlobalVariable(origName + "_affine_table", tableTy,
                                      new ConstantZero(tableTy));
    M->addGlobalVariable(tableG);
    auto* coefficientG = new GlobalVariable(
        origName + "_affine_coefficient", i32, new ConstantInt(0));
    M->addGlobalVariable(coefficientG);
    instrumentAffineCoefficient(body, summary, coefficientG);

    // Redirect recursive and external calls.  Keep external sites so a small
    // number of hot callers can receive one controlled wrapper expansion later.
    // Calls created below deliberately target body on a miss/overflow and are
    // therefore not part of this scan.
    std::vector<CallInst*> externalCalls;
    for (auto* g : M->getFunctions()) {
        if (g == wrapper) continue;
        for (auto* bb : g->getBody()->getBlocks())
            for (auto* inst : bb->getInstructions())
                if (auto* call = dyn_cast<CallInst>(inst))
                    if (call->getFunction() == body) {
                        if (g != body) externalCalls.push_back(call);
                        call->setOperand(0, wrapper);
                    }
    }

    Region* region = wrapper->getBody();
    auto* entry = new BasicBlock("entry", region);
    auto* check = new BasicBlock("check", region);
    std::vector<BasicBlock*> globalChecks;
    globalChecks.reserve(scalarGlobals.size());
    for (unsigned i = 0; i < scalarGlobals.size(); ++i)
        globalChecks.push_back(
            new BasicBlock("global_check" + std::to_string(i), region));
    auto* hit = new BasicBlock("hit", region);
    auto* miss = new BasicBlock("miss", region);
    auto* slow = new BasicBlock("slow", region);

    auto C = [](int value) { return new ConstantInt(value); };
    auto namedBin = [&](Instruction::OpID op, Value* lhs, Value* rhs,
                        BasicBlock* bb) -> Value* {
        auto* bin = new BinaryInst(op, lhs, rhs, bb);
        bin->setName(nm());
        return bin;
    };
    auto gep = [&](Value* base, Value* index, BasicBlock* bb) -> Value* {
        auto* ptr = new GetElementPtrInst(base, index, bb);
        ptr->setName(nm());
        return ptr;
    };
    auto namedLoad = [&](Value* ptr, BasicBlock* bb) -> Value* {
        auto* load = new LoadInst(ptr, bb);
        load->setName(nm());
        return load;
    };

    // Snapshot mutable scalar globals before entering the original body.  They
    // form an epoch-like secondary key at each dense index.
    std::vector<Value*> globalKeys;
    globalKeys.reserve(scalarGlobals.size());
    for (auto* global : scalarGlobals)
        globalKeys.push_back(namedLoad(global, entry));

    Value* denseKey = wrapperArgs[keyIndex];
    // capacity is a power of two.  In two's-complement i32,
    //   0 <= key < capacity  <=>  (key & -capacity) == 0.
    // This also rejects every negative key and is cheaper than two signed
    // comparisons followed by an AND.
    Value* highBits =
        namedBin(Instruction::And, denseKey, C(-capacity), entry);
    auto* inRange = new ICmpInst(ICmpInst::EQ, highBits, C(0), entry);
    inRange->setName(nm());
    new BranchInst(inRange, check, slow, entry);

    Value* entryIndex =
        namedBin(Instruction::Mul, denseKey, C(entryWords), check);
    Value* tableBase = gep(tableG, C(0), check);
    Value* entryPtr = gep(tableBase, entryIndex, check);
    Value* valid = namedLoad(entryPtr, check);
    auto* isValid = new ICmpInst(ICmpInst::NE, valid, C(0), check);
    isValid->setName(nm());
    new BranchInst(isValid,
                   globalChecks.empty() ? hit : globalChecks.front(), miss,
                   check);

    for (unsigned i = 0; i < globalKeys.size(); ++i) {
        BasicBlock* bb = globalChecks[i];
        Value* stored = namedLoad(gep(entryPtr, C(valueWords + i), bb), bb);
        auto* equal = new ICmpInst(ICmpInst::EQ, stored, globalKeys[i], bb);
        equal->setName(nm());
        BasicBlock* next =
            i + 1 == globalKeys.size() ? hit : globalChecks[i + 1];
        new BranchInst(equal, next, miss, bb);
    }

    Value* coefficient = compactCoefficient
                             ? namedBin(Instruction::Sub, valid, C(1), hit)
                             : namedLoad(gep(entryPtr, C(1), hit), hit);
    Value* bias = namedLoad(gep(entryPtr, C(biasOffset), hit), hit);
    Value* scaled = namedBin(Instruction::Mul, coefficient,
                             wrapperArgs[accumulatorIndex], hit);
    Value* hitResult = namedBin(Instruction::Add, scaled, bias, hit);
    new StoreInst(coefficient, coefficientG, hit);
    new ReturnInst(hitResult, hit);

    std::vector<Value*> zeroArgs = wrapperArgs;
    zeroArgs[accumulatorIndex] = C(0);
    auto* resultZero = new CallInst(body, zeroArgs, miss);
    resultZero->setName(nm());
    Value* missCoefficient = namedLoad(coefficientG, miss);
    Value* missScaled = namedBin(Instruction::Mul, missCoefficient,
                                 wrapperArgs[accumulatorIndex], miss);
    Value* missResult = namedBin(Instruction::Add, missScaled, resultZero, miss);

    // Nested recursive calls may have populated the same element.  Recompute
    // field pointers after both body calls and publish validity last.
    Value* missIndex =
        namedBin(Instruction::Mul, denseKey, C(entryWords), miss);
    Value* missBase = gep(tableG, C(0), miss);
    Value* missEntry = gep(missBase, missIndex, miss);
    for (unsigned i = 0; i < globalKeys.size(); ++i)
        new StoreInst(globalKeys[i],
                      gep(missEntry, C(valueWords + i), miss), miss);
    if (!compactCoefficient)
        new StoreInst(missCoefficient, gep(missEntry, C(1), miss), miss);
    new StoreInst(resultZero, gep(missEntry, C(biasOffset), miss), miss);
    Value* publishedTag = compactCoefficient
                              ? namedBin(Instruction::Add, missCoefficient,
                                         C(1), miss)
                              : C(1);
    new StoreInst(publishedTag, missEntry, miss);
    new ReturnInst(missResult, miss);

    auto* slowResult = new CallInst(body, wrapperArgs, slow);
    slowResult->setName(nm());
    new ReturnInst(slowResult, slow);

    // Normal inlining rejects the wrapper/body mutual-recursion SCC.  The
    // affine proof makes one expansion safe: cloned recursive edges target the
    // wrapper, so this removes one body call per newly discovered state without
    // recursively expanding code.  Keep very large bodies out of this path.
    if (SSAInline::countInsts(body) <= 100)
        SSAInline(M).inlineCallUnchecked(resultZero);

    // The wrapper is recursive only through the cloned body.  Expanding it
    // once at a few external sites removes the per-iteration ABI call while
    // recursive edges in the clone continue to target wrapper, so code growth
    // is finite and proportional to the number of external sites.
    if (externalCalls.size() <= 2 && SSAInline::countInsts(wrapper) <= 200)
        for (auto* call : externalCalls)
            SSAInline(M).inlineCallUnchecked(call);
    return true;
}

bool Memoize::transformHash(
    Function* f, const std::vector<GlobalVariable*>& scalarGlobals,
    bool allTailRecursive, const RecursiveAffineSummary* summary) {
    Type* i32 = Type::getIntTy();
    Function* body = f;
    std::string origName = body->getName();

    const bool hasAffineSummary = summary != nullptr;
    const int accumulatorIndex =
        hasAffineSummary ? static_cast<int>(summary->accumulatorIndex) : -1;
    if (hasAffineSummary) allTailRecursive = false;
    const unsigned keyCount = body->getArgs().size() + scalarGlobals.size() -
                              (hasAffineSummary ? 1u : 0u);
    // Ordinary entries are [valid, value, keys...].  Affine entries cache the
    // function result A*x+B as [valid, A, B, keys...].
    const unsigned valueWords = hasAffineSummary ? 3u : 2u;
    const unsigned entryWords = keyCount + valueWords;
    if (entryWords > static_cast<unsigned>(std::numeric_limits<int>::max() / 4))
        return false;

    const int entryBytes = static_cast<int>(entryWords) * 4;
    const int cacheBytes =
        hasAffineSummary ? affineHashCacheBytes : hashCacheBytes;
    const int maxEntries = cacheBytes / entryBytes;
    if (maxEntries == 0) return false;
    int capacity = 1;
    while (capacity <= maxEntries / 2)
        capacity <<= 1;

    body->setName(origName + "_memoFunc");

    Function* wrapper = new Function(origName, body->getType());
    std::vector<Value*> wrapperArgs;
    wrapperArgs.reserve(body->getArgs().size());
    for (unsigned i = 0; i < body->getArgs().size(); ++i) {
        auto* oldArg = body->getArgs()[i];
        auto* arg = new Argument(oldArg->getType(), oldArg->getName(), wrapper, i);
        wrapper->addArgument(arg);
        wrapperArgs.push_back(arg);
    }
    M->addFunction(wrapper);

    const int tableSize = capacity * static_cast<int>(entryWords);
    auto* tableTy = new ArrayType(i32, tableSize);
    auto* tableG = new GlobalVariable(origName + "_hash_table", tableTy,
                                      new ConstantZero(tableTy));
    M->addGlobalVariable(tableG);

    GlobalVariable* affineCoefficientG = nullptr;
    if (hasAffineSummary) {
        affineCoefficientG = new GlobalVariable(
            origName + "_affine_coefficient", i32, new ConstantInt(0));
        M->addGlobalVariable(affineCoefficientG);
        instrumentAffineCoefficient(body, *summary, affineCoefficientG);
    }

    GlobalVariable* pathDepthG = nullptr;
    GlobalVariable* pathIndexG = nullptr;
    Function* finish = nullptr;
    Argument* finishResult = nullptr;
    if (allTailRecursive) {
        pathDepthG = new GlobalVariable(origName + "_hash_path_depth", i32,
                                        new ConstantInt(0));
        M->addGlobalVariable(pathDepthG);
        auto* pathTy = new ArrayType(i32, hashPathCapacity);
        pathIndexG = new GlobalVariable(origName + "_hash_path_index", pathTy,
                                        new ConstantZero(pathTy));
        M->addGlobalVariable(pathIndexG);

        finish = new Function(origName + "_hash_finish", i32);
        finishResult = new Argument(i32, "result", finish, 0);
        finish->addArgument(finishResult);
        M->addFunction(finish);

        // Only terminal returns flush the recorded tail-call path.  Returns of
        // a direct recursive call remain tail calls and simply propagate the
        // already-flushed result.
        for (auto* bb : body->getBody()->getBlocks()) {
            std::vector<ReturnInst*> returns;
            for (auto* inst : bb->getInstructions())
                if (auto* ret = dyn_cast<ReturnInst>(inst))
                    returns.push_back(ret);
            for (auto* ret : returns) {
                Value* value = ret->getOperand(0);
                auto* call = dyn_cast<CallInst>(value);
                if (call && call->getFunction() == body) continue;

                auto& insts = bb->getInstructions();
                auto retIt = std::find(insts.begin(), insts.end(), ret);
                auto* flush = new CallInst(finish, {value}, bb);
                flush->setName(nm());
                insts.splice(retIt, insts, std::prev(insts.end()));
                ret->setOperand(0, flush);
            }
        }
    }

    // Redirect both recursive and external calls through the cache.  The miss
    // block below is created afterwards and intentionally targets body.
    for (auto* g : M->getFunctions()) {
        if (g == wrapper) continue;
        for (auto* bb : g->getBody()->getBlocks())
            for (auto* inst : bb->getInstructions())
                if (auto* call = dyn_cast<CallInst>(inst))
                    if (call->getFunction() == body)
                        call->setOperand(0, wrapper);
    }

    Region* region = wrapper->getBody();
    auto* entry = new BasicBlock("entry", region);
    std::vector<BasicBlock*> keyChecks;
    keyChecks.reserve(keyCount);
    for (unsigned i = 0; i < keyCount; ++i)
        keyChecks.push_back(new BasicBlock("check" + std::to_string(i), region));
    auto* hit = new BasicBlock("hit", region);
    auto* miss = new BasicBlock("miss", region);
    BasicBlock* record = nullptr;
    BasicBlock* overflow = nullptr;
    if (allTailRecursive) {
        record = new BasicBlock("record", region);
        overflow = new BasicBlock("overflow", region);
    }

    auto C = [](int v) { return new ConstantInt(v); };
    auto namedBin = [&](Instruction::OpID op, Value* lhs, Value* rhs,
                        BasicBlock* bb) -> Value* {
        auto* bin = new BinaryInst(op, lhs, rhs, bb);
        bin->setName(nm());
        return bin;
    };
    auto gep = [&](Value* base, Value* index, BasicBlock* bb) -> Value* {
        auto* ptr = new GetElementPtrInst(base, index, bb);
        ptr->setName(nm());
        return ptr;
    };
    auto namedLoad = [&](Value* ptr, BasicBlock* bb) -> Value* {
        auto* load = new LoadInst(ptr, bb);
        load->setName(nm());
        return load;
    };
    auto arrayElem = [&](GlobalVariable* array, Value* index,
                         BasicBlock* bb) -> Value* {
        return gep(gep(array, C(0), bb), index, bb);
    };
    auto emitHash = [&](const std::vector<Value*>& values,
                        BasicBlock* bb) -> Value* {
        Value* hash = C(0x13579bdf);
        for (auto* key : values) {
            hash = namedBin(Instruction::Mul, hash, C(1009), bb);
            hash = namedBin(Instruction::Xor, hash, key, bb);
        }
        auto* shifted = namedBin(Instruction::Ashr, hash, C(16), bb);
        return namedBin(Instruction::Xor, hash, shifted, bb);
    };

    if (allTailRecursive) {
        Region* finishRegion = finish->getBody();
        auto* finishEntry = new BasicBlock("entry", finishRegion);
        auto* finishHead = new BasicBlock("head", finishRegion);
        auto* finishBody = new BasicBlock("body", finishRegion);
        auto* finishExit = new BasicBlock("exit", finishRegion);

        Value* initialDepth = namedLoad(pathDepthG, finishEntry);
        new BranchInst(finishHead, finishEntry);

        auto* depth = new PhiInst(i32, finishHead);
        depth->setName(nm());
        depth->addIncoming(initialDepth, finishEntry);
        auto* hasEntry = new ICmpInst(ICmpInst::SGT, depth, C(0), finishHead);
        hasEntry->setName(nm());
        new BranchInst(hasEntry, finishBody, finishExit, finishHead);

        Value* nextDepth = namedBin(Instruction::Sub, depth, C(1), finishBody);
        Value* finishIndex = namedLoad(
            arrayElem(pathIndexG, nextDepth, finishBody), finishBody);
        Value* finishTableBase = gep(tableG, C(0), finishBody);
        Value* finishEntryPtr = gep(finishTableBase, finishIndex, finishBody);
        new StoreInst(finishResult, gep(finishEntryPtr, C(1), finishBody),
                      finishBody);
        new StoreInst(C(1), finishEntryPtr, finishBody);
        new BranchInst(finishHead, finishBody);
        depth->addIncoming(nextDepth, finishBody);

        new StoreInst(C(0), pathDepthG, finishExit);
        new ReturnInst(finishResult, finishExit);
    }

    // Materialize the complete key once in entry so that the same snapshot is
    // used by lookup and insertion.
    std::vector<Value*> keys;
    keys.reserve(keyCount);
    for (unsigned i = 0; i < wrapperArgs.size(); ++i)
        if (!hasAffineSummary || static_cast<int>(i) != accumulatorIndex)
            keys.push_back(wrapperArgs[i]);
    for (auto* gv : scalarGlobals)
        keys.push_back(namedLoad(gv, entry));

    // Full-key comparison, not hash uniqueness, provides correctness.
    Value* hash = emitHash(keys, entry);
    Value* slot = namedBin(Instruction::And, hash, C(capacity - 1), entry);
    Value* entryIndex = namedBin(Instruction::Mul, slot, C(entryWords), entry);
    Value* tableBase = gep(tableG, C(0), entry);
    Value* entryPtr = gep(tableBase, entryIndex, entry);

    // Empty slots take the miss edge without touching any key data.  Occupied
    // slots compare one component at a time and stop at the first mismatch.
    Value* valid = namedLoad(entryPtr, entry);
    auto* isValid = new ICmpInst(ICmpInst::NE, valid, C(0), entry);
    isValid->setName(nm());
    new BranchInst(isValid, keyChecks.empty() ? hit : keyChecks.front(), miss,
                   entry);

    for (unsigned i = 0; i < keys.size(); ++i) {
        BasicBlock* check = keyChecks[i];
        Value* keyPtr = gep(entryPtr, C(valueWords + i), check);
        Value* stored = namedLoad(keyPtr, check);
        auto* equal = new ICmpInst(ICmpInst::EQ, stored, keys[i], check);
        equal->setName(nm());
        BasicBlock* next = (i + 1 == keys.size()) ? hit : keyChecks[i + 1];
        new BranchInst(equal, next, miss, check);
    }

    Value* cached = namedLoad(gep(entryPtr, C(1), hit), hit);
    if (allTailRecursive) {
        auto* flushed = new CallInst(finish, {cached}, hit);
        flushed->setName(nm());
        new ReturnInst(flushed, hit);

        Value* pathDepth = namedLoad(pathDepthG, miss);
        auto* hasRoom = new ICmpInst(ICmpInst::SLT, pathDepth,
                                     C(hashPathCapacity), miss);
        hasRoom->setName(nm());
        new BranchInst(hasRoom, record, overflow, miss);

        // Reserve the slot without publishing it.  Nested tail states may
        // replace the same slot; they all receive the identical final result,
        // so retaining any one complete key is correct.
        new StoreInst(C(0), entryPtr, record);
        for (unsigned i = 0; i < keys.size(); ++i)
            new StoreInst(keys[i], gep(entryPtr, C(2 + i), record), record);
        new StoreInst(entryIndex, arrayElem(pathIndexG, pathDepth, record),
                      record);
        Value* nextDepth = namedBin(Instruction::Add, pathDepth, C(1), record);
        new StoreInst(nextDepth, pathDepthG, record);
        auto* recordedResult = new CallInst(body, wrapperArgs, record);
        recordedResult->setName(nm());
        new ReturnInst(recordedResult, record);

        auto* overflowResult = new CallInst(body, wrapperArgs, overflow);
        overflowResult->setName(nm());
        new ReturnInst(overflowResult, overflow);
    } else {
        if (hasAffineSummary) {
            Value* cachedBias = namedLoad(gep(entryPtr, C(2), hit), hit);
            Value* scaled = namedBin(Instruction::Mul, cached,
                                     wrapperArgs[accumulatorIndex], hit);
            Value* affineResult = namedBin(Instruction::Add, scaled, cachedBias,
                                           hit);
            new StoreInst(cached, affineCoefficientG, hit);
            new ReturnInst(affineResult, hit);
        } else {
            new ReturnInst(cached, hit);
        }

        Value* result = nullptr;
        Value* coefficient = nullptr;
        Value* bias = nullptr;
        if (hasAffineSummary) {
            std::vector<Value*> zeroArgs = wrapperArgs;
            zeroArgs[accumulatorIndex] = C(0);
            auto* resultZero = new CallInst(body, zeroArgs, miss);
            resultZero->setName(nm());
            bias = resultZero;
            coefficient = namedLoad(affineCoefficientG, miss);
            Value* scaled = namedBin(Instruction::Mul, coefficient,
                                     wrapperArgs[accumulatorIndex], miss);
            result = namedBin(Instruction::Add, scaled, bias, miss);
        } else {
            auto* ordinaryResult = new CallInst(body, wrapperArgs, miss);
            ordinaryResult->setName(nm());
            result = ordinaryResult;
        }
        auto storeFieldPtr = [&](unsigned offset) -> Value* {
            Value* fieldIndex = namedBin(Instruction::Add, entryIndex, C(offset),
                                         miss);
            return gep(tableBase, fieldIndex, miss);
        };
        for (unsigned i = 0; i < keys.size(); ++i)
            new StoreInst(keys[i], storeFieldPtr(valueWords + i), miss);
        if (hasAffineSummary) {
            new StoreInst(coefficient, storeFieldPtr(1), miss);
            new StoreInst(bias, storeFieldPtr(2), miss);
        } else {
            new StoreInst(result, storeFieldPtr(1), miss);
        }
        // Publish validity last, after the entire entry has been initialized.
        new StoreInst(C(1), entryPtr, miss);
        new ReturnInst(result, miss);
    }

    return true;
}

bool Memoize::run() {
    std::vector<Function*> funcs(M->getFunctions().begin(), M->getFunctions().end());
    bool changed = false;
    for (auto* f : funcs) {
        int iB = 0;
        if (isCandidate2D(f, iB))
            changed |= transform2D(f, iB);
        else if (isCandidate1D(f, iB))
            changed |= transform1D(f, iB);
        else {
            std::vector<GlobalVariable*> scalarGlobals;
            if (isHashCandidate(f, scalarGlobals)) {
                scalarGlobals.erase(
                    std::remove_if(
                        scalarGlobals.begin(), scalarGlobals.end(),
                        [&](GlobalVariable* global) {
                            return isStableScalarGlobal(f, global);
                        }),
                    scalarGlobals.end());
                RecursiveAffineSummary summary;
                if (RecursiveAffineAnalysis(f).run(summary)) {
                    if (f->getArgs().size() == 2)
                        changed |= transformAffineDense(
                            f, scalarGlobals, summary);
                    else
                        changed |= transformHash(
                            f, scalarGlobals, false, &summary);
                } else
                    changed |= transformHash(f, scalarGlobals,
                                             isAllTailRecursive(f));
            }
        }
    }
    return changed;
}
