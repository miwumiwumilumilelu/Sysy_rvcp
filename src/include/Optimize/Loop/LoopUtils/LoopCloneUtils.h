#ifndef LOOPCLONEUTILS_H
#define LOOPCLONEUTILS_H

#include "../../Analysis/LoopInfo.h"
#include "../../Scalar/IRClone.h"
#include <utility>
#include <vector>

namespace sysy {

struct LoopOneIterClone {
    BasicBlock* entry = nullptr;
    ValueMap valueMap;
    BlockMap blockMap;
    std::vector<std::pair<BasicBlock*, BasicBlock*>> exitEdges; // original pred, cloned pred
};

// Clone one execution of L. 
// Redirect the latch backedge to exitBB.
bool cloneLoopOneIteration(Loop* L, BasicBlock* exitBB, Region* region,
                           const ValueMap& initialMap,
                           LoopOneIterClone& out);

}

#endif
