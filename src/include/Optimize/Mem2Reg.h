#ifndef MEM2REG_H
#define MEM2REG_H

#include "IR/Module.h"
#include "Optimize/Dominators.h"
#include <map>
#include <vector>
#include <set>
#include <stack>

namespace sysy {

class Mem2Reg {
public:
    Mem2Reg(Module* m, Dominators* dom) : TheModule(m), Dom(dom) {}
    void run();

private:
    Module* TheModule;
    Dominators* Dom;
std::map<BasicBlock*, std::vector<BasicBlock*>> DomTreeChildren;

    std::vector<AllocaInst*> PromotableAllocas;

    std::map<PhiInst*, AllocaInst*> PhiToAlloca;

    std::map<AllocaInst*, std::vector<Value*>> IncomingVals;

    void promoteMemoryToRegister(Function* func, int& phiCounter);
    
    void buildDomTree(Function* func);
    void findPromotableAllocas(Function* func);
    bool isPromotable(AllocaInst* ai);

    void insertPhiNodes(Function* func, int& phiCounter);

    void rename(BasicBlock* bb);
};

}

#endif