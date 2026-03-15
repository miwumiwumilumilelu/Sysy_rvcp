#ifndef INSTSIMPLIFY_H
#define INSTSIMPLIFY_H

#include "IR/Module.h"
#include "IR/Instruction.h"

namespace sysy {

class InstSimplify {
public:
    InstSimplify(Module* m) : TheModule(m) {}
    bool run();

private:
    Module* TheModule;
    bool simplify(BasicBlock* bb);
};

}

#endif
