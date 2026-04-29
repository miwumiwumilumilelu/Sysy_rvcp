#include "Optimize/Scalar/TailCallElim.h"
#include "IR/Instruction.h"
#include "IR/Region.h"
#include <vector>

using namespace sysy;

bool TCE::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    
    // Record the position of tail recursion.
    // %r1 = call fun(...);
    // ret %r1;
    struct Site {
        CallInst* call;
        ReturnInst* ret;
        BasicBlock* bb;
    };
    std::vector<Site> sites;

    // Collect adjacent tail recursive nested call.
    for (auto* bb : f->getBody()->getBlocks()) {
        CallInst* pendingCall = nullptr;
        for (auto* inst : bb->getInstructions()) {
            if (auto* call = dyn_cast<CallInst>(inst)) {
                pendingCall = (call->getFunction() == f) ? call : nullptr;
            } else if (auto* ret = dyn_cast<ReturnInst>(inst)) {
                if (pendingCall && ret->getNumOperands() == 1 && ret->getOperand(0) == pendingCall)
                    sites.push_back({pendingCall, ret, bb});
                pendingCall = nullptr;
            } else {
                pendingCall = nullptr;
            }
        }
    }
    if (sites.empty()) return false;

    auto& args = f->getArgs();
    auto& blocks = f->getBody()->getBlocks();
    BasicBlock* oldEntry = blocks.front();

    // tce_pre:
    //  br oldEntry;
    // keep the loop normalized, just one preheader.
    auto* pre = new BasicBlock("tce_pre", nullptr);
    pre->setParent(f->getBody());
    new BranchInst(oldEntry, pre);
    blocks.push_front(pre);

    // oldEntry:
    //  ... use(arg1), use(arg2) ...
    // tail_bb:
    //  %x = call (new_arg1, new_arg2, ...);
    //  ret %x;
    // 
    // becomes:
    //
    // tce_pre:
    //   br oldEntry
    // oldEntry:
    //   tce_arg1 = phi [ arg1, tce_pre ], [ new_arg1, tail_bb ]
    //   tce_arg2 = phi [ arg2, tce_pre ], [ new_arg2, tail_bb ]
    //   ...
    // tail_bb:
    //   br oldEntry;
    std::vector<PhiInst*> phis;
    phis.reserve(args.size());
    auto& entryInsts = oldEntry->getInstructions();
    auto insertPos = entryInsts.begin();
    for (auto* arg : args) {
        auto* phi = new PhiInst(arg->getType(), nullptr);
        phi->setName("tce_" + arg->getName());
        phi->setParent(oldEntry);
        entryInsts.insert(insertPos, phi);
        phis.push_back(phi);
    }

    for (size_t i = 0; i < args.size(); i++) {
        args[i]->replaceAllUsesWith(phis[i]);
    }
    for (size_t i = 0; i < args.size(); i++) {
        phis[i]->addIncoming(args[i], pre);
    }

    for (auto& s : sites) {
        for (size_t i = 0; i < args.size(); i++)
            phis[i]->addIncoming(s.call->getOperand(i + 1), s.bb);

        s.ret->eraseInst();
        s.call->eraseInst();
        new BranchInst(oldEntry, s.bb);
    }

    return true;
}

bool TCE::run() {
    bool any = false;
    for (auto* f : M->getFunctions()) {
        any |= runFunc(f);
    }
    return any;
}
