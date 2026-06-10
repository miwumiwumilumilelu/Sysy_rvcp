#ifndef IRCLONE_H
#define IRCLONE_H

#include "../../IR/Instruction.h"
#include <map>
#include <unordered_map>

namespace sysy {

using ValueMap = std::map<Value*, Value*>;
using BlockMap = std::map<BasicBlock*, BasicBlock*>;

// Remap a value through vmap and basic blocks through bbMap.
Value* remapValue(Value* v, const ValueMap& vmap, const BlockMap& bbMap);

Instruction* cloneSkeleton(Instruction* src, BasicBlock* target);
void fillOperands(Instruction* clone, Instruction* src, const ValueMap& vmap, const BlockMap& bbMap);
Instruction* cloneInst(Instruction* src, BasicBlock* target, const ValueMap& vmap, const BlockMap& bbMap);

}

#endif
