#ifndef DCE_H
#define DCE_H

#include "../../IR/Module.h"

namespace sysy {

class DCE {
public:
    DCE(Module* m) : TheModule(m) {}
    bool run();
private:
    Module* TheModule;
    bool eliminateDeadCode(Function* func);
    bool isInstTrivallyDead(Instruction* inst);
};

}

#endif // DCE_H