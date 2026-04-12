#ifndef LOOPDELETIONUTILS_H
#define LOOPDELETIONUTILS_H

#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/LoopInfo.h"
#include "Optimize/Analysis/SCEV.h"
#include "IR/Instruction.h"
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace sysy {

bool isDeletionMaterializable(Value* v, Loop* L);

Value* materializeForDeletion(Value* v, Loop* L, BasicBlock* insertBB,
                        std::unordered_map<Value*, Value*>& cache,
                        std::set<Value*>& vis);

int evaluateDeletionCond(Value* cond, SCEV& scev);

Value* evaluateFirstIterValue(Value* v, Loop* L, BasicBlock* entryPred,
                        Dominators& dt,
                        std::vector<std::unique_ptr<ConstantInt>>& tempOwner,
                        std::unordered_map<Value*, Value*>& cache,
                        std::set<Value*>& vis);

}

#endif
