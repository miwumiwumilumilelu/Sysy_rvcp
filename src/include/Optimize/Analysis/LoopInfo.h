#ifndef LOOPINFO_H
#define LOOPINFO_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include <map>
#include <set>
#include <vector>

namespace sysy {
//   ┌─────────────────────────────────────────┐                                                                                                                                           
//   │  outer Loop (up)                        │                                                                                                                                           
//   │                                         │                                                                                                                                           
//   │   [pre]                                 │                                                                                                                                            
//   │    │      pre-header (dominates head)   │                                                                                                                                           
//   │    │                                    │                                                                                                                                           
//   │    ▼                                    │                                                                                                                                           
//   │   [head] ◄────────────────────────────  │ ◄── loop back-edge                                                                                                                         
//   │    │      loop header                   │     from latch                                                                                                                            
//   │    │                                    │                                                                                                                                           
//   │    │      ┌── inner Loop (sub[i])  ──┐  │                                                                                                                                            
//   │    ▼      │  [head']                 │  │                                                                                                                                           
//   │  [blocks] │  [blocks']               │  │                                                                                                                                           
//   │    │      │  [latch']                │  │                                                                                                                                           
//   │    │      └─────────────────────────-┘  │                                                                                                                                           
//   │    │                                    │                                                                                                                                           
//   │    ▼                                    │
//   │   [latch] ──────────────────────────►   │
//   │           back-edge to head             │
//   └─────────────────────────────────────────┘
struct Loop {
    // up is the outer loop containing this loop.
    // sub is the inner loops contained in this loop.
    BasicBlock* head = nullptr;
    BasicBlock* pre = nullptr;
    BasicBlock* latch = nullptr; 
    Loop* up = nullptr; 
    std::vector<BasicBlock*> blocks;
    std::vector<Loop*> sub; 

    bool has(BasicBlock* bb) const {
        for (auto b : blocks) if (b == bb) return true;
        return false;
    }
};

class LoopInfo {
public:
    LoopInfo(Function* f, Dominators& dt);
    ~LoopInfo() { for (auto l : All) delete l; }

    // Innermost loop containing bb, or nullptr.
    Loop* loopOf(BasicBlock* bb) const {
        auto it = BMap.find(bb);
        return it != BMap.end() ? it->second : nullptr;
    }
    // All Top level loops in f.
    const std::vector<Loop*>& tops() const { return Tops; }

private:
    Function* F;
    Dominators& DT;
    std::vector<Loop*> All;
    std::vector<Loop*> Tops;
    std::map<BasicBlock*, Loop*> BMap;

    void build();
    void ensurePre(Loop* L);
};

} // namespace sysy
#endif
