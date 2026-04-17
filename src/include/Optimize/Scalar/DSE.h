#ifndef DSE_H
#define DSE_H

#include "IR/Module.h"
#include "IR/Instruction.h"
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

    std::set<Value*> computeAliasSet(AllocaInst* alloca);
    bool isDeadAliasSet(const std::set<Value*>& aliasSet);

    void collectDeadStores(const std::set<Value*>& aliasSet,
                           std::set<Instruction*>& dead);
};

}

#endif
