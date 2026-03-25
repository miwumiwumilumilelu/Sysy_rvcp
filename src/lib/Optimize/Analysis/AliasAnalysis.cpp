#include "Optimize/Analysis/AliasAnalysis.h"
#include "Optimize/Analysis/SCEV.h"
#include "IR/Instruction.h"
#include "IR/Module.h"
#include "IR/Value.h"

using namespace sysy;

Value* AliasAnalysis::getBase(Value* ptr) {
    while (auto* gep = dyn_cast<GetElementPtrInst>(ptr))
        ptr = gep->getOperand(0);
    return ptr;
}

AliasAnalysis::Result AliasAnalysis::query(Value* a, Value* b, SCEV* scev) const {
    if (a == b) return Result::MustAlias;

    Value* ba = getBase(a);
    Value* bb = getBase(b);

    // Check if ba/bb are known physical memory allocations.
    bool knownA = isa<AllocaInst>(ba) || isa<GlobalVariable>(ba) || isa<Argument>(ba);
    bool knownB = isa<AllocaInst>(bb) || isa<GlobalVariable>(bb) || isa<Argument>(bb);

    if (knownA && knownB) {
        if (ba == bb) {
            // Use SCEV to prove distinct offsets -> NoAlias.
            if (scev && scev->distinct(scev->get(a), scev->get(b)))
                return Result::NoAlias;
            return Result::MayAlias;
        }
        if (isa<AllocaInst>(ba) || isa<AllocaInst>(bb)) return Result::NoAlias;
        if (isa<GlobalVariable>(ba) && isa<GlobalVariable>(bb)) return Result::NoAlias;
        // GlobalVariable vs Argument: f(&g) makes arg point to g.
        // or
        // Argument p vs Argument q: void f(int* p, int* q) { } called as f(&x, &x)
        return Result::MayAlias;
    }

    return Result::MayAlias;
}
