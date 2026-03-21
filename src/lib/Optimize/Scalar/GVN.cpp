#include "Optimize/Scalar/GVN.h"
#include "Optimize/Scalar/ExprKey.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Analysis/PureFunc.h"
#include "IR/Instruction.h"
#include <functional>
#include <map>
#include <vector>

using namespace sysy;

bool GVN::run() {
    bool any = false;
    purityCache.clear();
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool GVN::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;

    Dominators dt(f); 
    dt.run();

    std::map<BasicBlock*, std::vector<BasicBlock*>> domCh;
    for (auto bb : f->getBody()->getBlocks())
        domCh[bb] = {};
    for (auto bb : f->getBody()->getBlocks())
        if (auto* idom = dt.getIDom(bb)) domCh[idom].push_back(bb);

    std::map<ExprKey, Instruction*> exprTab;
    std::map<CallKey, Instruction*> callTab;
    bool any = false;

    std::function<void(BasicBlock*)> visit = [&](BasicBlock* bb) {
        std::vector<ExprKey> newExpr;
        std::vector<CallKey> newCall;
        std::vector<std::pair<Instruction*, Value*>> toReplace;

        for (auto inst : bb->getInstructions()) {
            auto op = inst->getOpID();
            if (op == Instruction::Phi || op == Instruction::Br  || op == Instruction::Ret  ||
                op == Instruction::Store|| op == Instruction::Load|| op == Instruction::Alloca)
                continue;

            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (call->getType()->isVoid() || !isPureFunc(call->getFunction(), purityCache))
                    continue;
                CallKey k;
                k.push_back((uint64_t)(uintptr_t)call->getFunction());
                for (int i = 1; i < (int)call->getNumOperands(); i++)
                    k.push_back(vnKey(call->getOperand(i)));
                auto it = callTab.find(k);
                if (it != callTab.end()) {
                    toReplace.push_back({inst, it->second});
                } else {
                    callTab[k] = inst; 
                    newCall.push_back(k);
                }
                continue;
            }

            ExprKey k = makeExprKey(inst);
            if (k == ExprKey{0, 0, 0}) continue;

            auto it = exprTab.find(k);
            if (it != exprTab.end()) {
                toReplace.push_back({inst, it->second});
            } else {
                exprTab[k] = inst; 
                newExpr.push_back(k);
            }
        }

        for (auto& [old, rep] : toReplace) {
            old->replaceAllUsesWith(rep);
            bb->getInstructions().remove(old);
            any = true;
        }

        for (auto child : domCh[bb]) visit(child);

        // DFS
        // Restore scope: remove entries added in this block.
        for (auto& k : newExpr) exprTab.erase(k);
        for (auto& k : newCall) callTab.erase(k);
    };

    visit(f->getEntryBlock());
    return any;
}
