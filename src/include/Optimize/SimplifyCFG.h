#ifndef SIMPLIFYCFG_H
#define SIMPLIFYCFG_H

#include "IR/Module.h"
#include <map>
#include <vector>

namespace sysy {

class SimplifyCFG {
public:
    SimplifyCFG(Module* m) : TheModule(m) {}
    void run();

private:
    Module* TheModule;
    bool simplifyFunction(Function* func);

    bool removeGhostPhiEdges(Region* region);
    bool simplifyBranches(Region* region);

    // Eliminate unreachable blocks.
    bool eliminateUnreachableBlocks(Region* region);
    // Merge basic blocks.
    bool mergeBasicBlocks(Region* region);
    // Eliminate empty blocks.
    bool eliminateEmptyBlocks(Region* region);

    std::map<BasicBlock*, std::vector<BasicBlock*>> computePredecessors(Region* region);
};

}

#endif