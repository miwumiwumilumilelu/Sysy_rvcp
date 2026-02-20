#ifndef REGALLOC_H
#define REGALLOC_H

#include "rv/MCModule.h"
#include <map>
#include <set>
#include <vector>
#include <algorithm>

namespace sysy {

// Describing the lifecycle of a virtual register.
struct Interval {
    // VRegID
    int vreg;
    // Using InstID.
    int start;
    int end;
    // Assigned physical register.
    PReg assigned;
    bool spilled;
    int stackOffset;

    bool isFloat;

    // Sort by start time.
    bool operator<(const Interval& t) const {
        return start < t.start;
    }
};

class RegAlloc {
public:
    void run(MCModule* m);

private:
    MCFunc* currFunc;
    std::vector<Interval*> intervals;

    std::vector<Interval*> active;

    // Physical register occupancy status (0 indicates free).
    std::map<PReg, Interval*> physRegState;

    void numberInstructions(MCFunc* f);
    void analyzeLiveness(MCFunc* f);
    void linearScan();
    void rewriteCode(MCFunc* f);

    std::map<MCBlk*, std::set<int>> liveIn;
    std::map<MCBlk*, std::set<int>> liveOut;
    std::map<MCBlk*, std::set<int>> def;
    std::map<MCBlk*, std::set<int>> use;

    std::map<MCInst*, int> instId; // numbering instructions.
    std::map<MCBlk*, int> blkStart, blkEnd;

    std::vector<int> callInstIds;
    std::unordered_map<std::string, MCBlk*> label2blk; // O(1) lookup

    void computeLocalLiveness(MCBlk* b);
    void computeGlobalLiveness(MCFunc* f);
    void buildIntervals(MCFunc* f);

    std::map<int, int> allocaOffsets;
};

}

#endif