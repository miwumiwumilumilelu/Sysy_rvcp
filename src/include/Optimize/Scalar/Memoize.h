#ifndef MEMOIZE_H
#define MEMOIZE_H

#include "../../IR/Module.h"
#include <string>

namespace sysy {

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
    // Follow a GEP-chain back to its underlying global, or null.
    GlobalVariable* traceToGlobal(Value* v);
public:
    Memoize(Module* m) : M(m) {}
    bool run();
};

}

#endif
