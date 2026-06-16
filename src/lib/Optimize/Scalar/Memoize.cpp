#include "../../../include/Optimize/Scalar/Memoize.h"
#include "../../../include/IR/Instruction.h"
#include "../../../include/IR/Value.h"
#include "../../../include/IR/Type.h"
#include <vector>

using namespace sysy;

// The runtime guard makes any choice safe
static const int limit = 1000;

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

bool Memoize::isCandidate(Function* f, int& iB) {
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

bool Memoize::transform(Function* f, int iB) {
    const int wB = limit;
    // 0, 1, ..., wB
    // f[i][w]
    const int stride = wB + 1;
    const int size = (iB + 1) * stride;

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
    //      inb = 1<=i<=iB && 1<=w<=wB;
    //      br inb, check, slow;
    auto* c0 = new ICmpInst(ICmpInst::SGE, argI, C(1), entry);
    c0->setName(nm());
    auto* c1 = new ICmpInst(ICmpInst::SLE, argI, C(iB), entry);
    c1->setName(nm());
    auto* c2 = new ICmpInst(ICmpInst::SGE, argW, C(1), entry); 
    c2->setName(nm());
    auto* c3 = new ICmpInst(ICmpInst::SLE, argW, C(wB), entry); 
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

bool Memoize::run() {
    std::vector<Function*> funcs(M->getFunctions().begin(), M->getFunctions().end());
    bool changed = false;
    for (auto* f : funcs) {
        int iB = 0;
        if (isCandidate(f, iB))
            changed |= transform(f, iB);
    }
    return changed;
}
