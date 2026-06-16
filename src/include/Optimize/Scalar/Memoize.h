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
    bool isCandidate(Function* f, int& iB);
    bool transform(Function* f, int iB);
    // Follow a GEP-chain back to its underlying global, or null.
    GlobalVariable* traceToGlobal(Value* v);
public:
    Memoize(Module* m) : M(m) {}
    bool run();
};

}

#endif
