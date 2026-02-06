#ifndef RVGEN_H
#define RVGEN_H

#include "IR/Module.h"
#include <string>
#include <sstream>
#include <map>
#include <vector>

namespace sysy {

class RVGen {
public:
    RVGen(Module* module);
    void generate();
    std::string getAssembly() const { return AsmStream.str(); }

private:
    Module* TheModule;
    std::stringstream AsmStream;

    std::map<Value*, int> StackSlots;
    int CurrentStackSize = 0;

    std::map<BasicBlock*, std::string> BBLabelMap;

    void emit(const std::string &inst);
    void emitLabel(const std::string &label);

    void allocateStackSlots(Function* func);
    void loadValueToReg(Value* val, const std::string &regName);
    void storeRegToStack(const std::string &regName, Value* destVal);

    void genFunction(Function* func);
    void genBasicBlock(BasicBlock* bb);
    void genInstruction(Instruction* inst);

    void assignLabels(Function* func);
};

}

#endif