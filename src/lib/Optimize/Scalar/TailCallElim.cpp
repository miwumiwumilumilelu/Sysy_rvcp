#include "../../../include/Optimize/Scalar/TailCallElim.h"
#include "../../../include/IR/Instruction.h"
#include "../../../include/IR/Region.h"
#include <vector>

using namespace sysy;

bool TCE::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    
    // Record the position of tail recursion.
    // %r1 = call fun(...);
    // ret %r1;
    struct Site {
        CallInst* call;
        BasicBlock* bb;
        ReturnInst* directRet = nullptr;
        BranchInst* forwardingBr = nullptr;
        PhiInst* forwardingPhi = nullptr;
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
                    sites.push_back({pendingCall, bb, ret, nullptr, nullptr});
                pendingCall = nullptr;
            } else {
                pendingCall = nullptr;
            }
        }
    }

    // Inlining commonly represents the same tail position through a shared
    // return block:
    //
    //   %r = call f(...)
    //   br merge
    // merge:
    //   %v = phi [ %r, tail_bb ], ...
    //   ret %v
    //
    // Recognize that forwarding edge as a tail call too.  Other incoming values
    // and the shared return remain intact.
    for (auto* bb : f->getBody()->getBlocks()) {
        auto& insts = bb->getInstructions();
        if (insts.size() < 2) continue;
        auto brIt = std::prev(insts.end());
        auto* br = dyn_cast<BranchInst>(*brIt);
        if (!br || br->getNumOperands() != 1) continue;
        auto* dest = dyn_cast<BasicBlock>(br->getOperand(0));
        if (!dest) continue;

        auto callIt = std::prev(brIt);
        auto* call = dyn_cast<CallInst>(*callIt);
        if (!call || call->getFunction() != f) continue;

        auto& destInsts = dest->getInstructions();
        if (destInsts.empty()) continue;
        auto* ret = dyn_cast<ReturnInst>(destInsts.back());
        if (!ret || ret->getNumOperands() != 1) continue;
        auto* phi = dyn_cast<PhiInst>(ret->getOperand(0));
        if (!phi || phi->getParent() != dest) continue;

        bool forwardsCall = false;
        for (int i = 0; i + 1 < phi->getNumOperands(); i += 2)
            if (phi->getOperand(i) == call && phi->getOperand(i + 1) == bb) {
                forwardsCall = true;
                break;
            }
        if (!forwardsCall) continue;

        bool simpleReturnBlock = true;
        for (auto* inst : destInsts)
            if (inst != ret && !isa<PhiInst>(inst)) {
                simpleReturnBlock = false;
                break;
            }
        if (!simpleReturnBlock) continue;

        sites.push_back({call, bb, nullptr, br, phi});
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

        if (s.directRet)
            s.directRet->eraseInst();
        if (s.forwardingPhi) {
            // The forwarding edge is being removed.  Keep every phi in the
            // shared return block consistent, including currently-dead phis
            // that cleanup has not removed yet.
            auto* dest = s.forwardingPhi->getParent();
            for (auto* inst : dest->getInstructions()) {
                auto* phi = dyn_cast<PhiInst>(inst);
                if (!phi) break;
                phi->removeIncomingByBlock(s.bb);
            }
        }
        if (s.forwardingBr)
            s.forwardingBr->eraseInst();
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
