#ifndef PHIELIM_H
#define PHIELIM_H

#include "rv/MCModule.h"

namespace sysy {

class PhiElim {
public:
    void run (MCModule *m);

private:
    void runOnFunc(MCFunc *f);
    bool isFloat(int vr, MCFunc *f);
    std::list<MCInst*>::iterator findPos(MCBlk* b);
};

}

#endif