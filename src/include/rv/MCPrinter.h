#ifndef MCPRINTER_H
#define MCPRINTER_H

#include "rv/MCModule.h"
#include <iostream>

namespace sysy {

class MCPrinter {
public:
    void print(MCModule* module, std::ostream& os);

private:
    void print(MCFunc* func, std::ostream& os);
    void print(MCBlk* blk, std::ostream& os);
    void print(MCInst* inst, std::ostream& os);
    void print(MCOpnd& opnd, std::ostream& os);

    const char* getOpcName(MCInst::Opc opc);
    const char* getRegName(PReg preg);
};

}

#endif