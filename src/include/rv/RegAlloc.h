#ifndef REGALLOC_H
#define REGALLOC_H

#include "rv/MCModule.h"
#include <map>
#include <set>
#include <vector>

namespace sysy {

struct Interval {
    int vreg;              // Virtual register ID
    int start;             // Start instruction ID
    int end;               // End instruction ID
    PReg assigned;         // Assigned physical register
    bool spilled;          // Spilled to stack?
    int stackOffset;       // Stack offset if spilled
    bool isFloat;          // Floating point register?
    MCInst* defInst;       // Defining instruction (for rematerialization)

    bool operator<(const Interval& other) const {
        return start < other.start;
    }
};

class RegAlloc {
public:
    void run(MCModule* m);

private:
    MCFunc* currFunc;
    std::vector<Interval*> intervals;
    std::vector<int> callInstIds;
    std::map<int, int> allocaOffsets;

    struct AllocState {
        std::map<PReg, Interval*> physRegState;
        std::vector<Interval*> active;
        int stackOffset;
    };

    AllocState state;

    void numberInstructions(MCFunc* f);
    void analyzeLiveness(MCFunc* f);
    void buildIntervals(MCFunc* f);

    std::map<MCBlk*, std::set<int>> liveIn, liveOut, def, use;
    std::map<MCInst*, int> instId;
    std::map<MCBlk*, int> blkStart, blkEnd;
    std::unordered_map<std::string, MCBlk*> label2blk;

    void computeLocalLiveness(MCBlk* b);
    void computeGlobalLiveness(MCFunc* f);

    void allocateRegisters();
    bool isLeafFunction() const;

    void rewriteProgram();
};

}

#endif
