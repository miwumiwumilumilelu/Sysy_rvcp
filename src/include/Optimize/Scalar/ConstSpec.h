#ifndef CONSTSPEC_H
#define CONSTSPEC_H

#include "IR/Module.h"
#include <map>
#include <set>
#include <vector>

namespace sysy {

class ConstSpec {
public:
    explicit ConstSpec(Module* m, int maxClones = 32)
        : TheModule(m), MaxClones(maxClones) {}
    bool run();

private:
    Module* TheModule;
    int MaxClones;
    int cloneCount = 0;

    // (argIndex, constValue) pairs for the args that are specialized.
    using ArgPattern = std::vector<std::pair<int, int>>;

    std::map<std::pair<Function*, ArgPattern>, Function*> cache;

    // Set of functions that are themselves specializations (never re-cloned).
    std::set<Function*> isSpec;

    // Build the constant-arg pattern for a call site (skips non-ConstantInt args).
    ArgPattern getPattern(CallInst* call);

    // Look up an existing or create a new specialization.
    // Returns nullptr if the clone cap is reached.
    Function* getOrCreate(Function* callee, const ArgPattern& pat);

    // Create a fresh specialized clone.
    Function* cloneWithConsts(Function* orig, const ArgPattern& pat);
};

}
#endif
