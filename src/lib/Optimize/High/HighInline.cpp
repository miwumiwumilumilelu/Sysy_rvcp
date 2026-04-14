#include "Optimize/High/HighInline.h"
#include "Optimize/Scalar/IRClone.h"
#include "Optimize/Scalar/SSAInline.h"
#include <algorithm>
#include <assert.h>
#include <vector>

using namespace sysy;

int HighInline::countInsts(Region* region) {
    int total = 0;
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            total++;
            for (auto& sub : inst->getRegions())
                total += countInsts(sub.get());
        }
    }
    return total;
}

void HighInline::collectReturns(Region* region, std::vector<ReturnInst*>& rets) {
    for (auto bb : region->getBlocks()) {
        for (auto inst : bb->getInstructions()) {
            if (auto ret = dyn_cast<ReturnInst>(inst))
                rets.push_back(ret);
            for (auto& sub : inst->getRegions())
                collectReturns(sub.get(), rets);
        }
    }
}

bool HighInline::atBack(Instruction* inst) {
    if (!inst || !inst->getParent()) return false;
    auto& insts = inst->getParent()->getInstructions();
    return !insts.empty() && insts.back() == inst;
}

Instruction* HighInline::tailParent(ReturnInst* ret) {
    if (!ret || !atBack(ret)) return nullptr;
    auto* bb = ret->getParent();
    if (!bb) return nullptr;
    auto* region = bb->getParent();
    if (!region) return nullptr;
    return region->getParentInst();
}

void HighInline::erase(Instruction* inst) {
    if (!inst) return;
    if (inst->getParent())
        inst->getParent()->getInstructions().remove(inst);
    inst->replaceAllUsesWith(nullptr);
    for (int i = 0; i < inst->getNumOperands(); ++i)
        inst->setOperand(i, nullptr);
    inst->setParent(nullptr);
    delete inst;
}

bool HighInline::foldTailReturns(Function* f) {
    if (!f || f->getBody()->getBlocks().empty()) return false;
    if (f->getBody()->getBlocks().size() != 1) return false;

    std::vector<ReturnInst*> rets;
    collectReturns(f->getBody(), rets);
    if (rets.size() != 2) return false;

    auto* p0 = tailParent(rets[0]);
    auto* p1 = tailParent(rets[1]);
    if (!p0 || p0 != p1) return false;

    auto* parent = p0;
    auto* entry = f->getBody()->getEntryBlock();
    if (!entry || entry->getInstructions().empty()) return false;
    if (entry->getInstructions().back() != parent) return false;

    auto* r0Region = rets[0]->getParent()->getParent();
    auto* r1Region = rets[1]->getParent()->getParent();
    if (!r0Region || !r1Region || r0Region == r1Region) return false;
    if (r0Region->getParentInst() != parent || r1Region->getParentInst() != parent)
        return false;

    auto& insts = entry->getInstructions();
    auto parentIt = std::find(insts.begin(), insts.end(), parent);
    if (parentIt == insts.end()) return false;

    AllocaInst* retAddr = nullptr;
    if (f->getType() && !f->getType()->isVoid()) {
        retAddr = new AllocaInst(f->getType(), nullptr);
        retAddr->setName(f->getName() + "_ret.addr");
        retAddr->setParent(entry);
        insts.insert(parentIt, retAddr);

        for (auto* ret : rets) {
            auto& blockInsts = ret->getParent()->getInstructions();
            auto retIt = std::find(blockInsts.begin(), blockInsts.end(), ret);
            auto* st = new StoreInst(ret->getOperand(0), retAddr, nullptr);
            st->setParent(ret->getParent());
            blockInsts.insert(retIt, st);
            erase(ret);
        }

        auto* ld = new LoadInst(retAddr, nullptr);
        ld->setName(f->getName() + "_ret");
        ld->setParent(entry);
        insts.push_back(ld);
        auto* newRet = new ReturnInst(ld, nullptr);
        newRet->setParent(entry);
        insts.push_back(newRet);
        return true;
    }

    for (auto* ret : rets)
        erase(ret);

    auto* newRet = new ReturnInst(nullptr, nullptr);
    newRet->setParent(entry);
    insts.push_back(newRet);
    return true;
}

bool HighInline::isInlineable(Function* f) const {
    if (!f) return false;
    if (f->getBody()->getBlocks().empty()) return false;
    if (SSAInline::isRecursive(f)) return false;
    if (f->getBody()->getBlocks().size() != 1) return false;
    if (countInsts(f->getBody()) > threshold) return false;

    auto* entry = f->getBody()->getEntryBlock();
    if (!entry || entry->getInstructions().empty()) return false;

    std::vector<ReturnInst*> rets;
    collectReturns(f->getBody(), rets);
    if (rets.size() != 1) return false;

    auto* last = entry->getInstructions().back();
    return last == rets.front();
}

Instruction* HighInline::cloneFlatInst(
    Instruction* inst,
    BasicBlock* target,
    const std::map<Value*, Value*>& vmap,
    const std::map<BasicBlock*, BasicBlock*>& bbMap) {
    switch (inst->getOpID()) {
        case Instruction::Add:  case Instruction::Sub:
        case Instruction::Mul:  case Instruction::Div:  case Instruction::Mod:
        case Instruction::FAdd: case Instruction::FSub:
        case Instruction::FMul: case Instruction::FDiv:
        case Instruction::ICmp:
        case Instruction::FCmp:
        case Instruction::SIToFP:
        case Instruction::FPToSI:
        case Instruction::Alloca:
        case Instruction::Load:
        case Instruction::Store:
        case Instruction::GetElementPtr:
        case Instruction::Call:
        case Instruction::Br:
            return cloneInst(inst, target, vmap, bbMap);
        case Instruction::Break:
            return new BreakInst(nullptr);
        case Instruction::Continue:
            return new ContinueInst(nullptr);
        case Instruction::Flow: {
            std::vector<Value*> vals;
            for (int i = 0; i < inst->getNumOperands(); ++i)
                vals.push_back(remapValue(inst->getOperand(i), vmap, bbMap));
            return new FlowInst(vals, nullptr);
        }
        case Instruction::Ret:
        case Instruction::Phi:
        case Instruction::If:
        case Instruction::While:
            return nullptr;
    }
    return nullptr;
}

void HighInline::appendInst(BasicBlock* bb, Instruction* inst) {
    if (!inst) return;
    inst->setParent(bb);
    bb->getInstructions().push_back(inst);
}

void HighInline::cloneRegion(Region* src,
                            Region* dst,
                            std::map<Value*, Value*>& vmap) {
    std::map<BasicBlock*, BasicBlock*> bbMap;
    struct PhiPair { PhiInst* orig; PhiInst* clone; };
    std::vector<PhiPair> pendingPhis;

    for (auto bb : src->getBlocks()) {
        auto* cloned = new BasicBlock(bb->getName(), dst);
        bbMap[bb] = cloned;
    }

    for (auto bb : src->getBlocks()) {
        auto* clonedBB = bbMap[bb];
        for (auto inst : bb->getInstructions()) {
            if (auto* phi = dyn_cast<PhiInst>(inst)) {
                auto* cphi = new PhiInst(phi->getType(), nullptr);
                cphi->setName(phi->getName());
                cphi->setParent(clonedBB);
                clonedBB->getInstructions().push_back(cphi);
                vmap[phi] = cphi;
                pendingPhis.push_back({phi, cphi});
            }
        }
    }

    for (auto bb : src->getBlocks()) {
        auto* clonedBB = bbMap[bb];
        for (auto inst : bb->getInstructions()) {
            if (isa<ReturnInst>(inst)) {
                assert(false && "HighInline only supports top-level final return");
            }
            if (isa<PhiInst>(inst))
                continue;

            Instruction* cloned = nullptr;
            if (inst->getOpID() == Instruction::If ||
                inst->getOpID() == Instruction::While) {
                cloned = cloneStructuredInst(inst, vmap);
            } else {
                cloned = cloneFlatInst(inst, clonedBB, vmap, bbMap);
            }

            if (cloned && cloned->getParent() != clonedBB)
                appendInst(clonedBB, cloned);
            if (cloned && !cloned->getType()->isVoid())
                vmap[inst] = cloned;
        }
    }

    for (auto [origPhi, cphi] : pendingPhis) {
        for (int i = 0; i < origPhi->getNumOperands(); i += 2) {
            auto* val = remapValue(origPhi->getOperand(i), vmap, bbMap);
            auto* bb = cast<BasicBlock>(
                remapValue(origPhi->getOperand(i + 1), vmap, bbMap));
            cphi->addIncoming(val, bb);
        }
    }
}

Instruction* HighInline::cloneStructuredInst(
    Instruction* inst,
    std::map<Value*, Value*>& vmap) {
    if (auto* ifInst = dyn_cast<IfInst>(inst)) {
        auto baseMap = vmap;
        std::map<BasicBlock*, BasicBlock*> emptyBBMap;
        auto* cloned = new IfInst(remapValue(ifInst->getOperand(0), vmap, emptyBBMap), nullptr);
        cloned->setName(ifInst->getName());
        if (ifInst->getElseRegion())
            cloned->addElseRegion();
        for (auto rv : ifInst->getResults()) {
            auto* newRV = cloned->createResult(rv->getType());
            newRV->setName(rv->getName());
            vmap[rv] = newRV;
            baseMap[rv] = newRV;
        }
        auto thenMap = baseMap;
        cloneRegion(ifInst->getThenRegion(), cloned->getThenRegion(), thenMap);
        if (ifInst->getElseRegion())
            cloneRegion(ifInst->getElseRegion(), cloned->getElseRegion(), baseMap);
        return cloned;
    }

    if (auto* whileInst = dyn_cast<WhileInst>(inst)) {
        auto baseMap = vmap;
        auto* cloned = new WhileInst(nullptr);
        cloned->setName(whileInst->getName());
        std::map<BasicBlock*, BasicBlock*> emptyBBMap;
        for (int i = 0; i < whileInst->getNumOperands(); ++i)
            cloned->addOperand(remapValue(whileInst->getOperand(i), vmap, emptyBBMap));
        for (auto rv : whileInst->getResults()) {
            auto* newRV = cloned->createResult(rv->getType());
            newRV->setName(rv->getName());
            vmap[rv] = newRV;
            baseMap[rv] = newRV;
        }
        auto condMap = baseMap;
        auto bodyMap = baseMap;
        cloneRegion(whileInst->getCondRegion(), cloned->getCondRegion(), condMap);
        cloneRegion(whileInst->getBodyRegion(), cloned->getBodyRegion(), bodyMap);
        return cloned;
    }

    assert(false && "expected structured high-level instruction");
    return nullptr;
}

void HighInline::doInline(CallInst* call) {
    Function* callee = call->getFunction();
    BasicBlock* callBB = call->getParent();
    auto& insts = callBB->getInstructions();
    auto callIt = std::find(insts.begin(), insts.end(), call);
    assert(callIt != insts.end());

    std::map<Value*, Value*> vmap;
    const auto& fargs = callee->getArgs();
    for (int i = 0; i < (int)fargs.size(); ++i)
        vmap[fargs[i]] = call->getOperand(i + 1);

    std::vector<Instruction*> clones;
    Value* retVal = nullptr;

    auto* entry = callee->getBody()->getEntryBlock();
    assert(entry && "HighInline expects non-empty callee");
    assert(!entry->getInstructions().empty() && "HighInline expects a final return");

    for (auto inst : entry->getInstructions()) {
        if (auto ret = dyn_cast<ReturnInst>(inst)) {
            if (ret->getNumOperands() > 0) {
                std::map<BasicBlock*, BasicBlock*> emptyBBMap;
                retVal = remapValue(ret->getOperand(0), vmap, emptyBBMap);
            }
            break;
        }

        Instruction* cloned = nullptr;
        if (inst->getOpID() == Instruction::If ||
            inst->getOpID() == Instruction::While) {
            cloned = cloneStructuredInst(inst, vmap);
        } else {
            std::map<BasicBlock*, BasicBlock*> emptyBBMap;
            cloned = cloneFlatInst(inst, callBB, vmap, emptyBBMap);
        }

        if (cloned && !cloned->getType()->isVoid())
            vmap[inst] = cloned;
        clones.push_back(cloned);
    }

    for (auto* cloned : clones) {
        if (!cloned) continue;
        if (cloned->getParent() != callBB)
            appendInst(callBB, cloned);
        auto inserted = std::find(insts.begin(), insts.end(), cloned);
        assert(inserted != insts.end());
        insts.splice(callIt, insts, inserted);
    }

    if (retVal)
        call->replaceAllUsesWith(retVal);

    insts.erase(callIt);
    call->setParent(nullptr);
    delete call;
}

bool HighInline::run() {
    bool anyChanged = false;
    bool changed;

    do {
        changed = false;

        for (auto func : M->getFunctions()) {
            if (foldTailReturns(func))
                changed = true;

            std::vector<CallInst*> toInline;
            for (auto bb : func->getBody()->getBlocks()) {
                for (auto inst : bb->getInstructions()) {
                    if (auto call = dyn_cast<CallInst>(inst)) {
                        if (isInlineable(call->getFunction()))
                            toInline.push_back(call);
                    }
                }
            }

            for (auto* call : toInline) {
                doInline(call);
                changed = true;
            }
        }

        anyChanged |= changed;
    } while (changed);

    return anyChanged;
}
