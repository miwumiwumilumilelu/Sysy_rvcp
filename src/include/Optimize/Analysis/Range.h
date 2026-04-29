#ifndef RANGE_H
#define RANGE_H

#include "IR/Module.h"
#include "Optimize/Analysis/Dominators.h"
#include <climits>
#include <map>
#include <vector>

namespace sysy {

class PhiInst;

// Integer range [low, high] (inclusive).
struct IRange {
    int low, high;
    static IRange scalar(int v) { return {v, v}; }
    static IRange unknown()     { return {INT_MIN, INT_MAX}; }
    bool operator==(const IRange& o) const { return low == o.low && high == o.high; }
    bool operator!=(const IRange& o) const { return !(*this == o); }
};

IRange rangeJoin(IRange l, IRange r, bool widen = false);
IRange rangeMeet(IRange l, IRange r);

class RangeAnalysis {
public:
    explicit RangeAnalysis(Function* f);

    bool has(Value* v) const;
    IRange get(Value* v) const;

    bool isNonNeg(Value* v) const; // low >= 0
    bool isPositive(Value* v) const; // low > 0

private:
    Function* F;
    Dominators dom;
    std::map<Value*, IRange> ranges;

    bool hasRange(Value* v) const;
    IRange getRange(Value* v) const;

    bool narrowConditional(PhiInst* phi, bool& changed);
    bool calculateRange(Instruction* inst, int nowiden);
    void doSplit();
    void undoSplit();
    void doAnalyze();

    std::vector<PhiInst*> splitPhis_;
};

}

#endif
