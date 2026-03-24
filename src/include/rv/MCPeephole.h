#ifndef MCPEEPHOLE_H
#define MCPEEPHOLE_H

#include "rv/MCFunction.h"
#include "rv/MCBlock.h"
#include "rv/RvOp.h"

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
