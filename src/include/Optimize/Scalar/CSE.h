#ifndef CSE_H
#define CSE_H

#include "IR/Module.h"
#include "IR/Instruction.h"
#include <map>
#include <tuple>

namespace sysy {

class CSE {
public:
    CSE(Module* m) : TheModule(m) {}
    bool run();

private:
    Module* TheModule;
    bool localCSE(BasicBlock* bb);
};

}

#endif
