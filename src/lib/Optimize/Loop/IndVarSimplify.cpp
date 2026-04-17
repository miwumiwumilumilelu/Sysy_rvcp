#include "Optimize/Loop/IndVarSimplify.h"
#include "Optimize/Analysis/Dominators.h"
#include "IR/Instruction.h"
#include <functional>

using namespace sysy;

bool IndVarSimplify::runOnLoop(Loop* L, Dominators& /*dt*/, SCEV& scev) {
    return unifyIndVars(L, scev);
}

bool IndVarSimplify::run() {
    bool any = false;
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool IndVarSimplify::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    Dominators dt(f); dt.run();
    LoopInfo li(f, dt);
    SCEV scev(f, li);

    bool changed = false;
    std::function<void(Loop*)> visit = [&](Loop* L) {
        for (auto sub : L->sub) visit(sub);
        changed |= unifyIndVars(L, scev);
    };
    for (auto top : li.tops()) visit(top);
    return changed;
}

bool IndVarSimplify::unifyIndVars(Loop* L, SCEV& scev) {
    if (!L->head) return false;

    std::vector<PhiInst*> phis;
    for (auto inst : L->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst);
        if (!phi) break;
        phis.push_back(phi);
    }
    if (phis.size() < 2) return false;

    std::vector<SE*> ses;
    ses.reserve(phis.size());
    for (auto* phi : phis)
        ses.push_back(scev.get(phi));

    bool any = false;
    std::vector<bool> dead(phis.size(), false);
    for (size_t i = 0; i < phis.size(); i++) {
        if (dead[i]) continue;
        if (isa<SEUnknown>(ses[i])) continue;
        for (size_t j = i + 1; j < phis.size(); j++) {
            if (dead[j]) continue;
            if (phis[i]->getType() != phis[j]->getType()) continue;
            if (!scev.equal(ses[i], ses[j])) continue;
            phis[j]->replaceAllUsesWith(phis[i]);
            dead[j] = true;
            any = true;
        }
    }

    for (int k = (int)phis.size() - 1; k >= 0; k--) {
        if (!dead[k]) continue;
        L->head->getInstructions().remove(phis[k]);
    }
    return any;
}
