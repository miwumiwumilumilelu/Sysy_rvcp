#ifndef LOOPROTATE_H
#define LOOPROTATE_H

#include "IR/Module.h"
#include "Optimize/Analysis/LoopInfo.h"

namespace sysy {

// while(cond){body} -> if(cond){do{body}while(cond)}
class LoopRotate {
    Module* M;

    // Single loop rotation.
    static bool runOnLoop(Loop* L, Function* f);

public:
    explicit LoopRotate(Module* m) : M(m) {}
    // Rotate all loops in one function.
    static bool runOnFunction(Function* f);
    bool run();
};

}
#endif
