#ifndef LOOPUNROLL_H
#define LOOPUNROLL_H

#include "../../IR/Module.h"

namespace sysy {

class LoopUnroll {
public:
    explicit LoopUnroll(Module* m, int threshold = 32)
        : TheModule(m), Threshold(threshold) {}
    bool run();
private:
    Module* TheModule;
    int Threshold;
    bool runFunc(Function* f);
};

}
#endif
