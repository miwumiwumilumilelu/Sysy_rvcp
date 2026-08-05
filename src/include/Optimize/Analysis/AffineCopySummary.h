#ifndef AFFINECOPYSUMMARY_H
#define AFFINECOPYSUMMARY_H

#include "LoopAffine.h"

namespace sysy {

struct AffineCopySummary {
    Function* function = nullptr;
    Value* base = nullptr;
    PhiInst* outerIV = nullptr;
    PhiInst* innerIV = nullptr;
    Value* outerBound = nullptr;
    Value* innerLimit = nullptr;
    LoadInst* sourceLoad = nullptr;
    StoreInst* destinationStore = nullptr;
    LoopAffineExpr sourceIndex;
    LoopAffineExpr destinationIndex;
};

// Summarize a pure, perfectly nested, lexicographically executed affine copy
// kernel.  Instruction order and names are irrelevant; address expressions
// are classified through LoopAffineAnalysis.
class AffineCopyAnalysis {
    Function* F;
public:
    explicit AffineCopyAnalysis(Function* function) : F(function) {}
    bool run(AffineCopySummary& summary);
};

}

#endif
