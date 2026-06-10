#ifndef LOWERFOR_H
#define LOWERFOR_H

#include "../../IR/Module.h"

namespace sysy {

// Lowers ForInst back to WhileInst before HighMem2Reg / FlattenCFG.
//
// ForInst(start, stop, step, ivAddr, pred) { 
//    body
// }
// 
// becomes:
// 
// store(ivAddr, start);
// while { cond: ICmp(pred, load(ivAddr), stop) } 
// do { 
//    body; 
//    store(ivAddr, load(ivAddr)+step) 
// }
class LowerFor {
    Module* M;

    bool runFunc(Function* f);
    bool processRegion(Region* region);
    bool processFor(ForInst* fi);

public:
    explicit LowerFor(Module* m) : M(m) {}
    bool run();
};

}

#endif
