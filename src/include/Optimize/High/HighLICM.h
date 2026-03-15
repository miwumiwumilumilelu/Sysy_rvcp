#ifndef HIGHLICM_H
#define HIGHLICM_H

#include "IR/Module.h"
#include "IR/Instruction.h"
#include <map>
#include <set>

namespace sysy {

class HighLICM {
public:
    HighLICM(Module* m) : TheModule(m) {}
    void run();

private:
    Module* TheModule;

    // Per-function set of global variables the function may (transitively) write.
    std::map<Function*, std::set<GlobalVariable*>> ModSets;
    void computeModSets();
    void collectDirectMods(Region* region, std::set<GlobalVariable*>& mods);

    void collectCallees(Region* region, std::set<Function*>& callees);
    // Collect all Values (Insts and results) defined inside region.
    void collectDefs(Region* region, std::set<Value*>& defs);
    void scanLoop(Region* region, std::set<Value*>& storeBases,
                std::set<Value*>& storeGepPtrs,
                std::set<GlobalVariable*>& callModGlobals, 
                bool& loopHasCall);
    // Returns true if inst can be safely hoisted out of the loop.
    bool isHoistable(Instruction* inst, const std::set<Value*>& loopDefs,
                     const std::set<Value*>& storeBases,
                     const std::set<Value*>& storeGepPtrs,
                     const std::set<GlobalVariable*>& callModGlobals,
                     bool loopHasCall);

    void processRegion(Region* region);
    void processWhile(WhileInst* whileInst);
};

} // namespace sysy

#endif
