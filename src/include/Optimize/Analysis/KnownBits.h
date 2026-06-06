#ifndef KNOWNBITS_H
#define KNOWNBITS_H

#include "IR/Module.h"
#include <map>

namespace sysy {

class NonNegAnalysis;

struct KBits {
    // x = 8 -> x = 000...00001000
    // bits proven to be 0.
    // zeros = ~0x00000008
    uint32_t zeros = 0;
    // bits proven to be 1.
    // ones = 0x00000008
    uint32_t ones  = 0;

    // True when no bit can be 1 in both *this and other simultaneously.
    // thus we can turn (a + b) to (a | b).
    bool disjointWith(const KBits& o) const {
        return (~zeros & ~o.zeros) == 0;
    }
};

// Computes KnownBits for Values via def-use.
class KnownBitsAnalysis {
    // Avoid redundant recursive calculations and recursion cycles.
    std::map<Value*, KBits> Cache;
    // Option to determine sign bit.
    const NonNegAnalysis* NonNeg = nullptr;
    KBits seed(Value* v) const;
public:
    KnownBitsAnalysis() = default;
    explicit KnownBitsAnalysis(const NonNegAnalysis* nonNeg) : NonNeg(nonNeg) {}

    KBits get(Value* v);
};

}

#endif
