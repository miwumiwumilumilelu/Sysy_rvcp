#ifndef MCFUNCTION_H
#define MCFUNCTION_H

#include "rv/MCInst.h"
#include "rv/MCBlock.h"
#include <vector>
#include <string>

namespace sysy {

class MCModule;

class MCFunc {
public:
    std::string name;
    std::vector<MCBlk*> blks;
    MCModule* parent;

    // Record how many virtual registers are used in this function.
    // For the next register allocation.
    int maxVReg; 

    MCFunc(std::string n, MCModule* p = nullptr) : name(n), parent(p), maxVReg(0) {}

    void add(MCBlk* b) {
        b->func = this;
        blks.push_back(b);
    }
};

}
#endif