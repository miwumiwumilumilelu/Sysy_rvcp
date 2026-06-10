#include "../../../include/Optimize/Scalar/DFE.h"
#include "../../../include/IR/Instruction.h"
#include <set>
#include <vector>

using namespace sysy;

bool DFE::run() {
    std::set<Function*> live;
    std::vector<Function*> worklist;

    auto mark = [&](Function* f) {
        if (!f || live.count(f)) return;
        live.insert(f);
        worklist.push_back(f);
    };

    for (auto* f : M->getFunctions()) {
        if (f->getName() == "main" || f->getBody()->getBlocks().empty())
            mark(f);
    }

    while (!worklist.empty()) {
        Function* f = worklist.back();
        worklist.pop_back();

        for (auto* bb : f->getBody()->getBlocks()) {
            for (auto* inst : bb->getInstructions()) {
                if (auto* call = dyn_cast<CallInst>(inst))
                    mark(call->getFunction());
            }
        }
    }

    bool changed = false;
    std::vector<Function*> funcs(M->getFunctions().begin(), M->getFunctions().end());
    for (auto* f : funcs) {
        if (!live.count(f) && !f->getBody()->getBlocks().empty()) {
            M->removeFunction(f);
            changed = true;
        }
    }

    return changed;
}
