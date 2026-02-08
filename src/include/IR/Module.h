#ifndef MODULE_H
#define MODULE_H

#include "IR/Value.h"
#include "IR/Instruction.h"
#include "IR/Region.h"
#include <list>
#include <vector>
#include <sstream>

namespace sysy {

class Function;
class BasicBlock;

class Argument : public Value {
public:
    Argument(Type *ty, const std::string &name, Function *f, int argNo)
        : Value(ty, name), Parent(f), ArgNo(argNo) {}
    std::string toString() const override { return Name; }

    Function* getParent() const { return Parent; }
    int getArgNo() const { return ArgNo; }

private:
    Function *Parent;
    int ArgNo;
};

class BasicBlock : public Value {
public:
    BasicBlock(const std::string &name, Region *parent);
    
    std::list<Instruction*>& getInstructions() { return InstList; }
    void addInstruction(Instruction *inst) { InstList.push_back(inst); }

    Region* getParent() const { return Parent; }
    Function* getParentFunc() const;

    std::string toString() const override;

private:
    std::list<Instruction*> InstList;
    Region *Parent;
};

class Function : public Value {
public:
    Function(const std::string &name, Type* retTy);

    Region* getBody() { return Body.get(); }
    BasicBlock* getEntryBlock();
    const std::vector<Argument*>& getArgs() const { return Args; }
    void addArgument(Argument *arg) { Args.push_back(arg); }

    std::string toString() const override;

private:
    std::unique_ptr<Region> Body;
    std::vector<Argument*> Args;
};

class Module {
public:
    void addFunction(Function *func) { Functions.push_back(func); }
    const std::vector<Function*>& getFunctions() const { return Functions; }

    Function* getFunction(const std::string &name) const {
        for (auto func : Functions) {
            if (func->getName() == name) return func;
        }
        return nullptr;
    }
    void addGlobalVariable(GlobalVariable *g) { Globals.push_back(g); }
    const std::vector<GlobalVariable*>& getGlobals() const { return Globals; }
    GlobalVariable* getGlobalVariable(const std::string &name) {
        for (auto g : Globals) if (g->getName() == name) return g;
        return nullptr;
    }
    std::string print();

private:
    std::vector<Function*> Functions;
    std::vector<GlobalVariable*> Globals;
};

}

#endif