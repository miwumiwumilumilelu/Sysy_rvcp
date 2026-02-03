#ifndef MODULE_H
#define MODULE_H

#include "IR/Value.h"
#include "IR/Instruction.h"
#include <list>
#include <vector>
#include <sstream>

namespace sysy {

class Function;
class BasicBlock;

class BasicBlock : public Value {
public:
    BasicBlock(const std::string &name, Function *parent);
    
    std::list<Instruction*>& getInstructions() { return InstList; }
    void addInstruction(Instruction *inst) { InstList.push_back(inst); }

    std::string toString() const override;

private:
    std::list<Instruction*> InstList;
    Function *Parent;
};

class Function : public Value {
public:
    Function(const std::string &name, Type* retTy)
        : Value(retTy, name) {}

    std::list<BasicBlock*>& getBlocks() { return Blocks; }
    void addBlock(BasicBlock *bb) { Blocks.push_back(bb); }

    std::string toString() const override;

private:
    std::list<BasicBlock*> Blocks;
};

class Module {
public:
    void addFunction(Function *func) { Functions.push_back(func); }
    std::string print();

private:
    std::list<Function*> Functions;
};

}

#endif