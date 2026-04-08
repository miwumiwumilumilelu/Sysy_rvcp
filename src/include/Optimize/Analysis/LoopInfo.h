#ifndef LOOPINFO_H
#define LOOPINFO_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include <map>
#include <set>
#include <vector>

namespace sysy {
//
//   up = outer loop containing this loop
//   sub[i] = nested inner loop directly contained in this loop
//
//   ┌────────────────────────────────────────────────────────────┐
//   │ outer loop (up)                                            │
//   │                                                            │
//   │   pre                                                      │
//   │    │   unique preheader outside the loop                   │
//   │    v                                                       │
//   │   head -------------------------------> exits[0]           │
//   │    │ \                              exit edge              │
//   │    │  \                                                   │
//   │    │   v                                                  │
//   │    │  exiting[0]                                          │
//   │    │     |                                                │
//   │    v     |                                                │
//   │  body blocks ...                                          │
//   │    │                                                      │
//   │    │      ┌──────── inner loop (sub[i]) ────────┐         │
//   │    │      │  head' -> ... -> latch' -> head'    │         │
//   │    │      │  blocks' / latches' / exits'        │         │
//   │    │      └──────────────────────────────────────┘         │
//   │    v                                                      │
//   │  latch  -----------------------------------------> head   │
//   │                                                            │
//   │  blocks   = all loop-internal basic blocks                │
//   │  latches  = all back-edge sources to head                 │
//   │  exiting  = loop blocks with a successor outside loop     │
//   │  exits    = outside successors reached from exiting       │
//   └────────────────────────────────────────────────────────────┘
//
struct Loop {
    // up is the outer loop containing this loop.
    // sub is the inner loops contained in this loop.
    BasicBlock* head = nullptr;
    BasicBlock* pre = nullptr;
    // Loop Simplify requires exactly one single latch.
    BasicBlock* latch = nullptr; 
    Loop* up = nullptr; 
    std::vector<BasicBlock*> blocks;
    std::vector<Loop*> sub; 
    std::vector<BasicBlock*> latches; 
    // loop-internal blocks with successors outside the loop.
    std::vector<BasicBlock*> exiting;
    // outside successor blocks reached from exiting.
    std::vector<BasicBlock*> exits;

    bool has(BasicBlock* bb) const {
        for (auto b : blocks) if (b == bb) return true;
        return false;
    }

    // Loop Simplify: unique preheader + single latch.
    bool hasPreheaderAndSingleLatch() const {
        return pre && latches.size() == 1;
    }
};

// Loop Simplify: Every exit block has only loop-internal preds.
inline bool hasDedicatedExits(Loop* L, Dominators& dt) {
    for (auto* exit : L->exits) {
        for (auto* pred : dt.getPredecessors(exit)) {
            if (!L->has(pred)) return false;
        }
    }
    return true;
}

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
    void buildPrehBB(Loop* L);
    void buildExits(Loop* L);
};

} // namespace sysy
#endif
