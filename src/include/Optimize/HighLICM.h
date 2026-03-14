#ifndef HIGHLICM_H
#define HIGHLICM_H

#include "IR/Module.h"
#include "IR/Instruction.h"
#include <set>

namespace sysy {

class HighLICM {
public:
    HighLICM(Module* m) : TheModule(m) {}
    void run();

private:
    Module* TheModule;

    // Collect all Values (Insts and results) defined inside region.
    void collectDefs(Region* region, std::set<Value*>& defs);
    // Collect store base pointers and set hasCall if any CallInst is found.
    void scanLoop(Region* region, std::set<Value*>& storeBases, bool& hasCall);
    // Returns true if inst can be safely hoisted out of the loop.
    bool isHoistable(Instruction* inst, const std::set<Value*>& loopDefs,
                     const std::set<Value*>& storeBases, bool hasCall);
    // Process all WhileInsts in region.
    void processRegion(Region* region);
    // Hoist loop-invariant insts from whileInst's body/cond to before it.
    void processWhile(WhileInst* whileInst);
};

} // namespace sysy

#endif
