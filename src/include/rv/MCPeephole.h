#ifndef MCPEEPHOLE_H
#define MCPEEPHOLE_H

#include "MCFunction.h"
#include "MCBlock.h"
#include "RvOp.h"

namespace sysy {
namespace rv {

class MCPeepholePass {
public:
    void run(MCFunction* func);

private:
    void runOnBlock(MCBlock* block);
    static bool eliminateTrivialBlocks(MCFunction* func);
    static bool isPure(RvOp* op);
};

} // namespace rv
} // namespace sysy

#endif
