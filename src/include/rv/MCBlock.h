#ifndef MCBLOCK_H
#define MCBLOCK_H

#include "rv/MCInst.h"
#include <list>
#include <string>

namespace sysy {

class MCFunc;

class MCBlk {
public:
    std::string name;
    std::list<MCInst*> insts;
    MCFunc* func;

    MCBlk(std::string n, MCFunc* f = nullptr) : name(n), func(f) {}

    void push(MCInst* i) {
        i->blk = this;
        insts.push_back(i);
    }

    void push_front(MCInst* i) {
        i->blk = this;
        insts.push_front(i);
    }
};

}

#endif 