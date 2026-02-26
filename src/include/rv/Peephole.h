#ifndef PEEPHOLE_H
#define PEEPHOLE_H

#include "rv/MCModule.h"
#include <list>

namespace sysy {

class Peephole {
public:
    void run(MCModule* m);

private:
    void optimizeFunction(MCFunc* func);
    void optimizeBlock(MCBlk* blk);

    // Optimize specific patterns
    bool eliminateRedundantMove(std::list<MCInst*>& insts);
    bool eliminateSelfMove(std::list<MCInst*>& insts);
};

}

#endif
