#include "../../../include/Optimize/Loop/LoopStrengthReduce.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "../../../include/IR/Instruction.h"
#include <functional>
#include <vector>

using namespace sysy;

static int typeBytes(Type* ty) {
    if (auto* arr = dyn_cast<ArrayType>(ty))
        return arr->getNumElements() * typeBytes(arr->getElementType());
    return 4;
}

bool LoopStrengthReduce::run() {
    bool any = false;
    for (auto f : M->getFunctions())
        any |= runFunc(f);
    return any;
}

bool LoopStrengthReduce::runFunc(Function* f) {
    if (f->getBody()->getBlocks().empty()) return false;
    Dominators dt(f); dt.run();
    LoopInfo li(f, dt);
    SCEV scev(f, li);

    bool changed = false;
    std::function<void(Loop*)> visit = [&](Loop* L) {
        for (auto sub : L->sub) visit(sub);
        changed |= runOnLoop(L, scev);
    };
    for (auto top : li.tops()) visit(top);
    return changed;
}

// Replace AddRec GEPs and Muls with phi recurrences.
bool LoopStrengthReduce::runOnLoop(Loop* L, SCEV& scev) {
    if (!L->pre || !L->head || !L->latch) return false;
    if (L->latches.size() != 1) return false;

    // Insert preheader instructions before the terminator.
    auto& preInsts = L->pre->getInstructions();
    auto preIns = std::prev(preInsts.end());

    // Insert latch instructions before the terminator.
    auto& latchInsts = L->latch->getInstructions();
    auto latchIns = std::prev(latchInsts.end());

    static int srID = 0;

    auto emitPre = [&](Instruction* inst) -> Instruction* {
        inst->setName("sr" + std::to_string(srID++));
        inst->setParent(L->pre);
        preInsts.insert(preIns, inst);
        return inst;
    };
    auto emitLatch = [&](Instruction* inst) -> Instruction* {
        inst->setName("sr" + std::to_string(srID++));
        inst->setParent(L->latch);
        latchInsts.insert(latchIns, inst);
        return inst;
    };

    // Expand loop-invariant SCEVs in the preheader.
    std::function<Value*(SE*)> tryExpandPre = [&](SE* se) -> Value* {
        if (auto* c = dyn_cast<SEConst>(se))
            return new ConstantInt((int)c->val);
        if (auto* u = dyn_cast<SEUnknown>(se))
            return isLoopInvariantValue(u->v, L) ? u->v : nullptr;
        if (auto* m = dyn_cast<SEMul>(se)) {
            Value* base = tryExpandPre(m->base);
            if (!base) return nullptr;
            if (m->factor == 1) return base;
            return emitPre(new BinaryInst(Instruction::Mul, base,
                                        new ConstantInt((int)m->factor), nullptr));
        }
        if (auto* a = dyn_cast<SEAdd>(se)) {
            Value* acc = nullptr;
            for (auto* op : a->ops) {
                Value* v = tryExpandPre(op);
                if (!v) return nullptr;
                acc = acc ? emitPre(new BinaryInst(Instruction::Add, acc, v, nullptr)) : v;
            }
            return acc ? acc : static_cast<Value*>(new ConstantInt(0));
        }
        return nullptr;
    };

    // Collect reducible instructions from loop blocks.
    struct GEPCandidate { 
        GetElementPtrInst* gep; 
        SEAddRec* idxAr; 
    };
    std::vector<GEPCandidate> gepCands;

    struct MulCandidate {
        BinaryInst* inst;
        SEAddRec* ar;
    };
    std::vector<MulCandidate> mulCands;

    struct SIToFPCandidate {
        CastInst* inst;
        SEAddRec* ar;
    };
    std::vector<SIToFPCandidate> sitofpCands;

    for (auto* bb : L->blocks) {
        for (auto* inst : bb->getInstructions()) {
            if (isa<PhiInst>(inst)) continue;

            if (auto* gep = dyn_cast<GetElementPtrInst>(inst)) {
                if (!isLoopInvariantValue(gep->getOperand(0), L)) continue;
                auto* idxAr = dyn_cast<SEAddRec>(scev.get(gep->getOperand(1)));
                if (!idxAr || idxAr->loop != L || idxAr->step == 0) continue;
                gepCands.push_back({gep, idxAr});
                continue;
            }

            if (auto* bin = dyn_cast<BinaryInst>(inst)) {
                if (bin->getOpID() != Instruction::Mul) continue;
                auto* ar = dyn_cast<SEAddRec>(scev.get(bin));
                if (!ar || ar->loop != L || ar->step == 0) continue;
                mulCands.push_back({bin, ar});
                continue;
            }

            if (auto* cast = dyn_cast<CastInst>(inst)) {
                if (cast->getOpID() != Instruction::SIToFP) continue;
                auto* ar = dyn_cast<SEAddRec>(scev.get(cast->getOperand(0)));
                if (!ar || ar->loop != L || ar->step == 0) continue;

                auto* startC = dyn_cast<SEConst>(ar->start);
                if (!startC) continue;
                ExitBranchInfo info;
                int64_t tc = -1;
                if (!analyzeExitBranch(L, L->latch, scev, info) ||
                    !getConstantTripCountFromInfo(info, L, tc) || tc <= 0) continue;

                constexpr int64_t kFloatExact = 1LL << 24;
                __int128 first = startC->val;
                __int128 last  = startC->val + (__int128)(tc - 1) * ar->step;
                __int128 lo = first < last ? first : last;
                __int128 hi = first < last ? last : first;
                if (lo < -kFloatExact || hi > kFloatExact) continue;
                sitofpCands.push_back({cast, ar});
            }
        }
    }

    if (gepCands.empty() && mulCands.empty() && sitofpCands.empty()) return false;

    bool changed = false;

    // Reduce GEP to pointer recurrences.
    for (auto& [gep, idxAr] : gepCands) {
        Value* startIdx = tryExpandPre(idxAr->start);
        if (!startIdx) continue;

        Value* base = gep->getOperand(0);
        Type* indexedTy = gep->getIndexedType();
        auto* resultPtrTy = dyn_cast<PointerType>(gep->getType());
        if (!indexedTy || !resultPtrTy) continue;

        int indexedBytes = typeBytes(indexedTy);
        int resultElemBytes = typeBytes(resultPtrTy->getPointeeType());
        if (resultElemBytes == 0) continue;
        int64_t byteStep = idxAr->step * static_cast<int64_t>(indexedBytes);
        if (byteStep % resultElemBytes != 0) continue;
        int adjustedStep = static_cast<int>(byteStep / resultElemBytes);

        // Emit the initial pointer in the preheader.
        auto* initGep = cast<GetElementPtrInst>(emitPre(new GetElementPtrInst(base, startIdx, nullptr)));
        initGep->setIndexedType(indexedTy);

        // Create the pointer phi in the header.
        auto* ptrPhi = new PhiInst(gep->getType(), nullptr);
        ptrPhi->setName("sr_ptr" + std::to_string(srID++));
        ptrPhi->setParent(L->head);
        ptrPhi->addIncoming(initGep, L->pre);
        L->head->getInstructions().push_front(ptrPhi);

        // Emit the next pointer on the latch.
        auto* nextPtr = emitLatch(new GetElementPtrInst(ptrPhi,
                                new ConstantInt(adjustedStep), nullptr));
        ptrPhi->addIncoming(nextPtr, L->latch);

        // Replace the original GEP with the recurrence.
        gep->replaceAllUsesWith(ptrPhi);
        gep->eraseInst();

        changed = true;
    }

    // Reduce integer multiplications to value recurrences.
    for (auto& [inst, ar] : mulCands) {
        Value* initVal = tryExpandPre(ar->start);
        if (!initVal) continue;

        // Create the value phi in the header.
        auto* valPhi = new PhiInst(inst->getType(), nullptr);
        valPhi->setName("sr_val" + std::to_string(srID++));
        valPhi->setParent(L->head);
        valPhi->addIncoming(initVal, L->pre);
        L->head->getInstructions().push_front(valPhi);

        // Emit the next value on the latch.
        Value* nextVal = emitLatch(new BinaryInst(Instruction::Add, valPhi,
                                                new ConstantInt((int)ar->step), nullptr));
        valPhi->addIncoming(nextVal, L->latch);

        inst->replaceAllUsesWith(valPhi);
        inst->eraseInst();

        changed = true;
    }

    // Reduce sitofp to float recurrences.
    for (auto& [inst, ar] : sitofpCands) {
        Value* intStart = tryExpandPre(ar->start);
        if (!intStart) continue;

        auto* floatStart = emitPre(new CastInst(Instruction::SIToFP, intStart, Type::getFloatTy(), nullptr));

        auto* floatPhi = new PhiInst(Type::getFloatTy(), nullptr);
        floatPhi->setName("sr_f" + std::to_string(srID++));
        floatPhi->setParent(L->head);
        floatPhi->addIncoming(floatStart, L->pre);
        L->head->getInstructions().push_front(floatPhi);

        auto* nextFloat = emitLatch(new BinaryInst(Instruction::FAdd, floatPhi, new ConstantFloat((float)ar->step), nullptr));
        floatPhi->addIncoming(nextFloat, L->latch);

        inst->replaceAllUsesWith(floatPhi);
        inst->eraseInst();
        changed = true;
    }

    return changed;
}
