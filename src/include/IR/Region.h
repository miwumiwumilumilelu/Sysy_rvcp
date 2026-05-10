#ifndef REGION_H
#define REGION_H

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace sysy {

class BasicBlock;
class Instruction;
class Function;

class Region {
public:
    Region(Instruction* parentInst = nullptr) : ParentInst(parentInst) {}
    Region(Function* parentFunc) : ParentFunc(parentFunc) {}

    using iterator = std::list<BasicBlock*>::iterator;
    iterator begin() { return Blocks.begin(); }
    iterator end() { return Blocks.end(); }
    
    std::list<BasicBlock*>& getBlocks() { return Blocks; }
    BasicBlock* getEntryBlock();

    void addBlock(BasicBlock* bb);
    void removeBlock(BasicBlock* bb);

    // Deep-clone all blocks from this region into dst, remapping values via vmap.
    void clone(Region* dst, std::map<Value*, Value*>& vmap);

    Instruction* getParentInst() const { return ParentInst; }
    Function* getParentFunc() const { return ParentFunc; }

private:
    std::list<BasicBlock*> Blocks;
    Instruction* ParentInst = nullptr;
    Function* ParentFunc = nullptr; 
};

}

#endif