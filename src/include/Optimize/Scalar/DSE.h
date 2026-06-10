#ifndef DSE_H
#define DSE_H

#include "../../IR/Module.h"
#include "../../IR/Instruction.h"
#include <set>
#include <vector>

namespace sysy {

class DSE {
public:
    DSE(Module* m) : TheModule(m) {}
    bool run();
private:
    Module* TheModule;

    bool runOnFunction(Function* func);
    bool runStoreLiveness(Function* func);
    bool runLocalPeepholes(Function* func);
    std::set<Value*> computeDerivedPointers(Value* base);
    bool collectDeadStoresToUnreadGlobal(GlobalVariable* glob,
                                         std::set<Instruction*>& dead);
    bool runUnreadGlobalStores();
};

}

#endif
