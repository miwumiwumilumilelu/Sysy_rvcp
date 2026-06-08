#include "Optimize/Scalar/SSAInline.h"
#include "Optimize/Scalar/IRClone.h"
#include "Optimize/Analysis/Dominators.h"
#include <algorithm>
#include <assert.h>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace sysy;

static int SSAInlineID = 0;

static std::string inlineName(const std::string& seed, int siteID) {
    if (!seed.empty())
        return seed + ".i" + std::to_string(siteID);
    return "%i" + std::to_string(siteID);
}

static void renameClone(Instruction* c, int siteID) {
    if (c && !c->getType()->isVoid())
        c->setName(inlineName(c->getName(), siteID));
}

bool SSAInline::isRecursive(Function* f) {
    for (auto bb : f->getBody()->getBlocks())
        for (auto inst : bb->getInstructions())
            if (auto call = dyn_cast<CallInst>(inst))
                if (call->getFunction() == f)
                    return true;
    return false;
}

int SSAInline::countInsts(Function* f) {
    int n = 0;
    for (auto bb : f->getBody()->getBlocks())
        n += (int)bb->getInstructions().size();
    return n;
}

static std::vector<BasicBlock*> successorsOf(BasicBlock* bb) {
    std::vector<BasicBlock*> succs;
    if (bb->getInstructions().empty()) return succs;
    auto* br = dyn_cast<BranchInst>(bb->getInstructions().back());
    if (!br) return succs;

    int start = (br->getNumOperands() == 1 ? 0 : 1);
    for (int i = start; i < br->getNumOperands(); ++i) {
        if (auto* target = dyn_cast<BasicBlock>(br->getOperand(i)))
            succs.push_back(target);
    }
    return succs;
}

static bool hasLoop(Function* f) {
    Dominators dom(f);
    dom.run();
    for (auto bb : f->getBody()->getBlocks()) {
        for (auto* succ : successorsOf(bb))
            if (dom.dominates(succ, bb))
                return true;
    }
    return false;
}

static bool isDerivedFromGlobal(Value* v) {
    if (isa<GlobalVariable>(v)) return true;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return isDerivedFromGlobal(gep->getOperand(0));
    return false;
}

static bool writesGlobal(Function* f) {
    for (auto bb : f->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                if (isDerivedFromGlobal(st->getOperand(1)))
                    return true;
            }
        }
    }
    return false;
}

static std::set<BasicBlock*> naturalLoopBlocks(Function* f) {
    std::map<BasicBlock*, std::vector<BasicBlock*>> preds;
    for (auto bb : f->getBody()->getBlocks()) {
        preds[bb];
        for (auto* succ : successorsOf(bb))
            preds[succ].push_back(bb);
    }

    Dominators dom(f);
    dom.run();

    std::set<BasicBlock*> loopBlocks;
    for (auto latch : f->getBody()->getBlocks()) {
        for (auto* header : successorsOf(latch)) {
            if (!dom.dominates(header, latch)) continue;

            std::vector<BasicBlock*> stack{latch};
            loopBlocks.insert(header);
            while (!stack.empty()) {
                BasicBlock* bb = stack.back();
                stack.pop_back();
                if (!loopBlocks.insert(bb).second) continue;
                for (auto* pred : preds[bb]) {
                    if (pred != header)
                        stack.push_back(pred);
                }
            }
        }
    }
    return loopBlocks;
}

template <typename Fn>
static void forEachInst(Function* f, Fn fn) {
    for (auto* bb : f->getBody()->getBlocks())
        for (auto* inst : bb->getInstructions())
            fn(inst);
}

static bool derivedFrom(Value* v, Value* base) {
    if (v == base) return true;
    if (auto* gep = dyn_cast<GetElementPtrInst>(v))
        return derivedFrom(gep->getOperand(0), base);
    return false;
}

static std::set<int> pow2DivModArgs(Function* f) {
    std::set<int> result;
    if (!f || f->getBody()->getBlocks().empty()) return result;

    std::map<Value*, std::set<int>> deps;
    const auto& args = f->getArgs();
    for (int i = 0; i < (int)args.size(); ++i)
        if (args[i]->getType()->isInt())
            deps[args[i]].insert(i);

    bool changed = true;
    while (changed) {
        changed = false;
        forEachInst(f, [&](Instruction* inst) {
            std::set<int> cur;
            for (int i = 0; i < inst->getNumOperands(); ++i) {
                auto it = deps.find(inst->getOperand(i));
                if (it != deps.end())
                    cur.insert(it->second.begin(), it->second.end());
            }
            auto& dst = deps[inst];
            size_t old = dst.size();
            dst.insert(cur.begin(), cur.end());
            changed |= dst.size() != old;
        });
    }

    forEachInst(f, [&](Instruction* inst) {
        auto* bin = dyn_cast<BinaryInst>(inst);
        if (!bin) return;
        if (bin->getOpID() != Instruction::Div &&
            bin->getOpID() != Instruction::Mod)
            return;
        auto* rhs = dyn_cast<ConstantInt>(bin->getOperand(1));

        auto log2v = [&](int v) -> int {
            if (v > 1 && (v & (v - 1)) == 0)
                return __builtin_ctz(static_cast<unsigned>(v));
            return -1;
        };

        if (!rhs || log2v(rhs->getValue()) < 0) return;

        auto it = deps.find(bin->getOperand(0));
        if (it != deps.end())
            result.insert(it->second.begin(), it->second.end());
    });
    return result;
}

// Parameters info for the fast path.
struct hasFastPath {
    Function* callee = nullptr;
    int ptrArg = -1;
    int lenArg = -1;
};

static hasFastPath findFastPath(Function* f) {
    hasFastPath t;
    if (!f || f->getBody()->getBlocks().empty()) return t;
    if (SSAInline::isRecursive(f)) return t;

    auto impurePtr = [&](Function* f, Value* base) -> bool{
        bool bad = false;
        forEachInst(f, [&](Instruction* inst) {
            if (bad) return;
            if (auto* st = dyn_cast<StoreInst>(inst)) {
                bad = derivedFrom(st->getOperand(1), base) || derivedFrom(st->getOperand(0), base);
                return;
            }
            if (auto* call = dyn_cast<CallInst>(inst)) {
                for (int i = 1; i < call->getNumOperands(); ++i)
                    bad |= derivedFrom(call->getOperand(i), base) &&
                        call->getOperand(i)->getType()->isPointer();
            }
        });
        return bad;
    };

    auto hasProfitableArrayLoad = [&](Function* f, int ptrArg) -> bool {
        std::map<Function*, std::set<int>> neededArgs;
        Value* base = f->getArgs()[ptrArg];

        for (auto* user : base->getUsers()) {
            (void)user;
        }

        for (auto* bb : f->getBody()->getBlocks()) {
            for (auto* inst : bb->getInstructions()) {
                auto* load = dyn_cast<LoadInst>(inst);
                if (!load || !derivedFrom(load->getOperand(0), base))
                    continue;

                for (auto* user : load->getUsers()) {
                    auto* call = dyn_cast<CallInst>(user);
                    if (!call || !call->getFunction()) continue;
                    auto& need = neededArgs[call->getFunction()];
                    if (need.empty())
                        need = pow2DivModArgs(call->getFunction());
                    for (int i = 1; i < call->getNumOperands(); ++i)
                        if (call->getOperand(i) == load && need.count(i - 1))
                            return true;
                }
            }
        }
        return false;
    };

    auto hasLoadUnderLen = [&](Function* f, Value* base, Value* len) -> bool {
        auto isIVUnderLen = [&](auto&& self, Value* idx) -> bool {
            auto* phi = dyn_cast<PhiInst>(idx);
            if (!phi || !len) return false;

            if (phi->getNumOperands() == 2)
                return self(self, phi->getOperand(0));

            BasicBlock* head = phi->getParent();
            if (!head || head->getInstructions().empty()) return false;
            auto* br = dyn_cast<BranchInst>(head->getInstructions().back());
            if (!br || br->getNumOperands() != 3) return false;

            auto* cmp = dyn_cast<ICmpInst>(br->getOperand(0));
            return cmp && cmp->getPredicate() == ICmpInst::SLT &&
                cmp->getOperand(0) == phi && cmp->getOperand(1) == len;
        };

        bool found = false;
        forEachInst(f, [&](Instruction* inst) {
            if (found) return;
            auto* load = dyn_cast<LoadInst>(inst);
            if (!load) return;

            auto* gep = dyn_cast<GetElementPtrInst>(load->getOperand(0));
            if (!gep || !derivedFrom(gep->getOperand(0), base)) return;
            if (isIVUnderLen(isIVUnderLen, gep->getOperand(1)))
                found = true;
        });
        return found;
    };

    const auto& args = f->getArgs();
    for (int p = 0; p < args.size(); p++) {
        if (!args[p]->getType()->isPointer()) continue;
        if (impurePtr(f, args[p])) continue;
        if (!hasProfitableArrayLoad(f, p)) continue;

        for (int l = 0; l < args.size(); l++) {
            if (!args[l]->getType()->isInt()) continue;
            if (!hasLoadUnderLen(f, args[p], args[l])) continue;

            return {f, p, l};
        }
    }
    return t;
}

static Function* cloneWithArrayFact(Module* m, Function* f, int ptrArg,
                                    int lenArg, CallInst* site) {
    std::string name = f->getName() + ".nnarr" + std::to_string(ptrArg);
    for (int i = 0; site && i < (int)f->getArgs().size(); ++i) {
        if (auto* ci = dyn_cast<ConstantInt>(site->getOperand(i + 1)))
            name += ".c" + std::to_string(i) + "_" + std::to_string(ci->getValue());
    }
    if (auto* old = m->getFunction(name)) return old;

    auto* clone = new Function(name, f->getType());
    clone->setNoInline();

    ValueMap vmap;
    BlockMap bbMap;
    std::vector<BasicBlock*> origBlocks;

    const auto& args = f->getArgs();
    for (int i = 0; i < (int)args.size(); ++i) {
        auto* a = new Argument(args[i]->getType(), args[i]->getName(), clone, i);
        clone->addArgument(a);
        if (site) {
            if (auto* ci = dyn_cast<ConstantInt>(site->getOperand(i + 1))) {
                vmap[args[i]] = ci;
                continue;
            }
        }
        vmap[args[i]] = a;
    }

    for (auto* bb : f->getBody()->getBlocks()) {
        origBlocks.push_back(bb);
        auto* cb = new BasicBlock(bb->getName(), nullptr);
        cb->setParent(clone->getBody());
        clone->getBody()->addBlock(cb);
        bbMap[bb] = cb;
    }
    for (auto* bb : origBlocks)
        for (auto* inst : bb->getInstructions())
            vmap[inst] = cloneSkeleton(inst, bbMap[bb]);
    for (auto* bb : origBlocks)
        for (auto* inst : bb->getInstructions())
            fillOperands(cast<Instruction>(vmap[inst]), inst, vmap, bbMap);

    clone->addNonNegFact(vmap[args[ptrArg]], vmap[args[lenArg]]);
    m->addFunction(clone);
    return clone;
}

struct GuardInfo {
    Value* base = nullptr;
    Value* len = nullptr;
    Value* ok = nullptr;
};

static void retargetPhiPreds(const std::vector<BasicBlock*>& succs,
                            BasicBlock* oldPred, BasicBlock* newPred) {
    for (auto* succ : succs) {
        for (auto* inst : succ->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            for (int i = 1; i < phi->getNumOperands(); i += 2)
                if (phi->getOperand(i) == oldPred)
                    phi->setOperand(i, newPred);
        }
    }
}

static GuardInfo insertNonNegScanBeforeCall(CallInst* call, Value* base, Value* len) {
    GuardInfo g;
    if (!call || !base || !len) return g;
    g.base = base;
    g.len = len;

    BasicBlock* bb = call->getParent();
    Region* region = bb->getParent();
    auto& blocks = region->getBlocks();
    auto insertPos = std::next(std::find(blocks.begin(), blocks.end(), bb));

    auto* head = new BasicBlock(bb->getName() + ".nn.head", nullptr);
    auto* body = new BasicBlock(bb->getName() + ".nn.body", nullptr);
    auto* cont = new BasicBlock(bb->getName() + ".nn.cont", nullptr);
    for (auto* nb : {head, body, cont}) {
        nb->setParent(region);
        blocks.insert(insertPos, nb);
    }

    auto origSuccs = successorsOf(bb);
    auto& insts = bb->getInstructions();
    auto callIt = std::find(insts.begin(), insts.end(), call);
    cont->getInstructions().splice(cont->getInstructions().begin(), insts,
                                   callIt, insts.end());
    for (auto* inst : cont->getInstructions())
        inst->setParent(cont);
    retargetPhiPreds(origSuccs, bb, cont);

    new BranchInst(head, bb);

    auto* iv = new PhiInst(Type::getIntTy(), head);
    auto* ok = new PhiInst(Type::getIntTy(), head);
    auto* more = new ICmpInst(ICmpInst::SLT, iv, g.len, head);
    new BranchInst(more, body, cont, head);

    auto* ptr = new GetElementPtrInst(g.base, iv, body);
    auto* val = new LoadInst(ptr, body);
    auto* ge0 = new ICmpInst(ICmpInst::SGE, val, new ConstantInt(0), body);
    auto* okNext = new BinaryInst(Instruction::And, ok, ge0, body);
    auto* ivNext = new BinaryInst(Instruction::Add, iv, new ConstantInt(1), body);
    new BranchInst(head, body);

    iv->addIncoming(new ConstantInt(0), bb);
    iv->addIncoming(ivNext, body);
    ok->addIncoming(new ConstantInt(1), bb);
    ok->addIncoming(okNext, body);

    g.ok = ok;
    return g;
}

static bool versionCallWithGuard(CallInst* call, Function* fast,
                                 const GuardInfo& guard) {
    if (!call || !fast || !guard.ok || call->getType()->isVoid())
        return false;

    BasicBlock* callBB = call->getParent();
    Region* region = callBB->getParent();
    auto& blocks = region->getBlocks();
    auto insertPos = std::next(std::find(blocks.begin(), blocks.end(), callBB));

    auto* fastBB = new BasicBlock(callBB->getName() + ".nn.fast", nullptr);
    auto* slowBB = new BasicBlock(callBB->getName() + ".nn.slow", nullptr);
    auto* mergeBB = new BasicBlock(callBB->getName() + ".nn.merge", nullptr);
    for (auto* nb : {fastBB, slowBB, mergeBB}) {
        nb->setParent(region);
        blocks.insert(insertPos, nb);
    }

    auto origSuccs = successorsOf(callBB);
    auto& insts = callBB->getInstructions();
    auto callIt = std::find(insts.begin(), insts.end(), call);
    mergeBB->getInstructions().splice(mergeBB->getInstructions().begin(), insts,
                                      std::next(callIt), insts.end());
    for (auto* inst : mergeBB->getInstructions())
        inst->setParent(mergeBB);
    retargetPhiPreds(origSuccs, callBB, mergeBB);

    std::vector<Value*> args;
    for (int i = 1; i < call->getNumOperands(); ++i)
        args.push_back(call->getOperand(i));

    auto* fc = new CallInst(fast, args, fastBB);
    fc->setName(call->getName() + ".nn");
    new BranchInst(mergeBB, fastBB);

    auto* sc = new CallInst(call->getFunction(), args, slowBB);
    sc->setName(call->getName());
    new BranchInst(mergeBB, slowBB);

    auto* phi = new PhiInst(call->getType(), nullptr);
    phi->setName(call->getName());
    phi->addIncoming(fc, fastBB);
    phi->addIncoming(sc, slowBB);
    phi->setParent(mergeBB);
    mergeBB->getInstructions().push_front(phi);

    call->replaceAllUsesWith(phi);
    call->eraseInst();
    new BranchInst(guard.ok, fastBB, slowBB, callBB);
    return true;
}

bool SSAInline::isInlineable(CallInst* call, bool callSiteInLoop) const {
    Function* f = call ? call->getFunction() : nullptr;
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;
    if (f->isNoInline()) return false;
    if (isRecursive(f)) return false;
    if (callSiteInLoop && hasLoop(f) && writesGlobal(f)) return false;
    if (countInsts(f) > threshold) return false;
    return true;
}

// callBB -> nextBBs
//
// after inline:
//
// callBB -> callee_bb0 -> callee_bb1 -> ... -> callee_bbN -> endBB -> nextBBs
void SSAInline::doInline(CallInst* call) {
    int siteID = SSAInlineID++;
    Function* callee = call->getFunction();
    BasicBlock* callBB = call->getParent();
    Region* callRegion = callBB->getParent();

    std::vector<BasicBlock*> origSuccs;
    if (!callBB->getInstructions().empty()) {
        if (auto br = dyn_cast<BranchInst>(callBB->getInstructions().back())) {
            int start = (br->getNumOperands() == 1 ? 0 : 1);
            for (int i = start; i < br->getNumOperands(); ++i)
                if (auto* succ = dyn_cast<BasicBlock>(br->getOperand(i)))
                    origSuccs.push_back(succ);
        }
    }

    BasicBlock* endBB = new BasicBlock(inlineName(callee->getName() + "_end", siteID), nullptr);

    // Move everything after the call into endBB.
    {
        auto& callInsts = callBB->getInstructions();
        auto callIt = std::find(callInsts.begin(), callInsts.end(), call);
        assert(callIt != callInsts.end());
        auto afterCall = std::next(callIt);
        endBB->getInstructions().splice(
            endBB->getInstructions().begin(), callInsts, afterCall, callInsts.end());
        for (auto inst : endBB->getInstructions())
            inst->setParent(endBB);
    }

    // Pre-create cloned blocks for the callee and splice them after callBB.
    BlockMap bbMap;
    std::vector<BasicBlock*> calleeBlocks;
    for (auto bb : callee->getBody()->getBlocks()) {
        auto* cloned = new BasicBlock(inlineName(callee->getName() + "_" + bb->getName(), siteID), nullptr);
        bbMap[bb] = cloned;
        calleeBlocks.push_back(bb);
    }
    {
        auto& blocks = callRegion->getBlocks();
        auto insPos = std::next(std::find(blocks.begin(), blocks.end(), callBB));
        for (auto origBB : calleeBlocks) {
            auto* c = bbMap[origBB];
            c->setParent(callRegion);
            blocks.insert(insPos, c);
        }
        endBB->setParent(callRegion);
        blocks.insert(insPos, endBB);
    }

    // Map callee arguments to caller operands.
    ValueMap vmap;
    const auto& fargs = callee->getArgs();
    for (int i = 0; i < (int)fargs.size(); ++i)
        vmap[fargs[i]] = call->getOperand(i + 1);

    // Two-pass clone: skeletons first so forward refs resolve when filling.
    // Branches and Returns are deferred — they drive the inlined control flow.
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret ||
                inst->getOpID() == Instruction::Br) continue;
            auto* c = cloneSkeleton(inst, clonedBB);
            renameClone(c, siteID);
            vmap[inst] = c;
        }
    }
    for (auto origBB : calleeBlocks) {
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret ||
                inst->getOpID() == Instruction::Br) continue;
            fillOperands(cast<Instruction>(vmap[inst]), inst, vmap, bbMap);
        }
    }

    // Route returns through endBB. Clone branches verbatim.
    std::vector<std::pair<Value*, BasicBlock*>> returns;
    for (auto origBB : calleeBlocks) {
        auto* clonedBB = bbMap[origBB];
        for (auto inst : origBB->getInstructions()) {
            if (inst->getOpID() == Instruction::Ret) {
                Value* rv = (inst->getNumOperands() > 0)
                            ? remapValue(inst->getOperand(0), vmap, bbMap)
                            : nullptr;
                returns.push_back({rv, clonedBB});
                new BranchInst(endBB, clonedBB);
            } else if (inst->getOpID() == Instruction::Br) {
                cloneInst(inst, clonedBB, vmap, bbMap);
            }
        }
    }

    // Hook return values.
    if (!returns.empty()) {
        if (returns.size() == 1) {
            call->replaceAllUsesWith(returns[0].first);
        } else {
            auto* phi = new PhiInst(callee->getType(), nullptr);
            if (!call->getName().empty())
                phi->setName(call->getName());
            for (auto [val, bb] : returns)
                phi->addIncoming(val, bb);
            phi->setParent(endBB);
            endBB->getInstructions().push_front(phi);
            call->replaceAllUsesWith(phi);
        }
    }

    // Remove the original call and branch from callBB into the first clone.
    {
        auto& callInsts = callBB->getInstructions();
        auto callIt = std::find(callInsts.begin(), callInsts.end(), call);
        assert(callIt != callInsts.end());
        call->eraseInst();

        auto* firstClone = bbMap[callee->getBody()->getEntryBlock()];
        new BranchInst(firstClone, callBB);
    }

    // Retarget phi incomings in successors: callBB is replaced by endBB on the
    // fallthrough path.
    for (auto* succ : origSuccs) {
        for (auto inst : succ->getInstructions()) {
            auto* phi = dyn_cast<PhiInst>(inst);
            if (!phi) break;
            for (int i = 1; i < phi->getNumOperands(); i += 2)
                if (phi->getOperand(i) == callBB)
                    phi->setOperand(i, endBB);
        }
    }
}

void SSAInline::AllocaHoist(Function* func) {
    if (!func || func->getBody()->getBlocks().empty()) return;
    BasicBlock* entry = func->getEntryBlock();
    if (!entry) return;

    auto& entryInsts = entry->getInstructions();
    auto insertPos = entryInsts.begin();
    while (insertPos != entryInsts.end() && isa<AllocaInst>(*insertPos))
        ++insertPos;

    std::vector<AllocaInst*> allocas;
    for (auto bb : func->getBody()->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto alloca = dyn_cast<AllocaInst>(inst)) {
                if (alloca->getParent() != entry ||
                    std::find(entryInsts.begin(), insertPos, alloca) == entryInsts.end())
                    allocas.push_back(alloca);
            }
        }
    }
    for (auto* alloca : allocas) {
        BasicBlock* parent = alloca->getParent();
        if (!parent) continue;
        auto& insts = parent->getInstructions();
        auto it = std::find(insts.begin(), insts.end(), alloca);
        if (it == insts.end()) continue;
        entryInsts.splice(insertPos, insts, it);
        alloca->setParent(entry);
    }
}

bool SSAInline::runFastPath(Module* m) {
    std::map<Function*, hasFastPath> targets;
    for (auto* f : m->getFunctions()) {
        auto t = findFastPath(f);
        if (t.callee)
            targets[f] = t;
    }
    if (targets.empty()) return false;

    bool changed = false;
    std::vector<Function*> funcs(m->getFunctions().begin(), m->getFunctions().end());
    for (auto* f : funcs) {
        std::vector<std::pair<CallInst*, hasFastPath>> calls;
        forEachInst(f, [&](Instruction* inst) {
            auto* call = dyn_cast<CallInst>(inst);
            if (!call || !call->getFunction()) return;
            auto it = targets.find(call->getFunction());
            if (it == targets.end()) return;
            calls.push_back({call, it->second});
        });

        for (auto [call, t] : calls) {
            if (!call->getParent()) continue;
            if (call->getNumOperands() <= std::max(t.ptrArg, t.lenArg) + 1) continue;

            Value* callBase = call->getOperand(t.ptrArg + 1);
            Value* callLen = call->getOperand(t.lenArg + 1);
            if (!callBase->getType()->isPointer() || !callLen->getType()->isInt())
                continue;

            t.callee->setNoInline();
            auto* fast = cloneWithArrayFact(m, t.callee, t.ptrArg, t.lenArg, call);
            GuardInfo guard = insertNonNegScanBeforeCall(call, callBase, callLen);
            if (!guard.ok) continue;

            changed |= versionCallWithGuard(call, fast, guard);
        }
    }

    return changed;
}

bool SSAInline::run() {
    bool anyChanged = false;
    bool changed;
    do {
        changed = false;
        for (auto func : M->getFunctions()) {
            std::vector<CallInst*> toInline;
            auto loopBlocks = naturalLoopBlocks(func);
            for (auto bb : func->getBody()->getBlocks())
                for (auto inst : bb->getInstructions())
                    if (auto call = dyn_cast<CallInst>(inst))
                        if (isInlineable(call, loopBlocks.count(bb)))
                            toInline.push_back(call);

            for (auto call : toInline) {
                doInline(call);
                changed = true;
            }
        }
        anyChanged |= changed;
    } while (changed);

    for (auto func : M->getFunctions())
        AllocaHoist(func);

    return anyChanged;
}
