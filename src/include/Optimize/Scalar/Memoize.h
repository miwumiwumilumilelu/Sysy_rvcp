#ifndef MEMOIZE_H
#define MEMOIZE_H

#include "../../IR/Module.h"
#include <string>
#include <vector>

namespace sysy {

struct RecursiveAffineSummary;

class Memoize {
    Module* M;
    int cnt = 0;
    std::string nm() { return "mz" + std::to_string(cnt++); }

    // Recognize a memoizable function and sets iB = bound for param0.
    bool isCandidate2D(Function* f, int& iB);
    bool transform2D(Function* f, int iB);
    // single i32 param, table indexed directly by it.
    bool isCandidate1D(Function* f, int& iB);
    bool transform1D(Function* f, int iB);
    // Generic fallback for recursive functions with i32 scalar keys.  
    // Mutable scalar globals read by the function are included in the cache key.
    bool isHashCandidate(Function* f,
                         std::vector<GlobalVariable*>& scalarGlobals);
    bool isAllTailRecursive(Function* f);
    bool isStableScalarGlobal(Function* f, GlobalVariable* global);
    bool transformHash(Function* f,
                       const std::vector<GlobalVariable*>& scalarGlobals,
                       bool allTailRecursive,
                       const RecursiveAffineSummary* summary = nullptr);
    // Dense summary cache for one explicit state argument.  Runtime guards
    // preserve correctness outside the selected table capacity.
    bool transformAffineDense(
        Function* f, const std::vector<GlobalVariable*>& scalarGlobals,
        const RecursiveAffineSummary& summary);
    void instrumentAffineCoefficient(Function* body,
                                     const RecursiveAffineSummary& summary,
                                     GlobalVariable* coefficientGlobal);
    // Follow a GEP-chain back to its underlying global, or null.
    GlobalVariable* traceToGlobal(Value* v);
public:
    Memoize(Module* m) : M(m) {}
    bool run();
};

}

#endif
