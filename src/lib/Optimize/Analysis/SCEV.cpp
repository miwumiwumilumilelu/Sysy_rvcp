#include "Optimize/Analysis/SCEV.h"
#include "IR/Instruction.h"
#include <algorithm>

using namespace sysy;

static int elemBytes(Type* pointeeTy) {
    if (auto* arr = dyn_cast<ArrayType>(pointeeTy))
        return arr->getNumElements() * elemBytes(arr->getElementType());
    return 4;
}

SE* SCEV::neg(SE* s) {
    if (auto* c = dyn_cast<SEConst>(s)) return own(new SEConst(-c->val));
    if (auto* m = dyn_cast<SEMul>(s)) return own(new SEMul(m->base, -m->factor));
    if (auto* a = dyn_cast<SEAdd>(s)) {
        std::vector<SE*> ops;
        for (auto op : a->ops) ops.push_back(neg(op));
        return own(new SEAdd(ops));
    }
    if (auto* ar = dyn_cast<SEAddRec>(s)) return own(new SEAddRec(neg(ar->start), -ar->step, ar->loop));
    return own(new SEMul(s, -1));
}

SE* SCEV::simplAdd(std::vector<SE*>& ops) {
    // Collect all addends (recurse into nested SEAdd).
    std::vector<SE*> flat;
    std::function<void(SE*)> flatten = [&](SE* s) {
        if (auto* a = dyn_cast<SEAdd>(s))
            for (auto op : a->ops) flatten(op);
        else
            flat.push_back(s);
    };
    for (auto op : ops) flatten(op);

    // 2x -> <*x,2>
    std::map<SE*, int64_t> coeffs;
    int64_t constSum = 0;
    for (auto s : flat) {
        if (auto* c = dyn_cast<SEConst>(s)) { constSum += c->val; continue; }
        int64_t cf = 1;
        SE* base = s;
        if (auto* m = dyn_cast<SEMul>(s)) { cf = m->factor; base = m->base; }
        coeffs[base] += cf;
    }

    std::vector<SE*> result;
    if (constSum != 0) result.push_back(own(new SEConst(constSum)));
    for (auto& [base, cf] : coeffs) {
        if (cf == 0) continue;
        if (cf == 1) result.push_back(base);
        else result.push_back(own(new SEMul(base, cf)));
    }

    if (result.empty()) return own(new SEConst(0));
    if (result.size()==1) return result[0];
    return own(new SEAdd(result));
}

// Check if loop inner is strictly nested inside loop outer.
static bool isInnerLoop(Loop* inner, Loop* outer) {
    for (Loop* L = inner->up; L; L = L->up)
        if (L == outer) return true;
    return false;
}

SE* SCEV::add(SE* a, SE* b) {
    auto* arA = dyn_cast<SEAddRec>(a);
    auto* arB = dyn_cast<SEAddRec>(b);
    if (arA && arB) {
        // Check if is the Same-loop merge: 
        // {s1,+,st1} + {s2,+,st2} = {s1+s2,+,st1+st2}
        if (arA->loop == arB->loop)
            return own(new SEAddRec(add(arA->start, arB->start), arA->step + arB->step, arA->loop));
        // Align the AddRec hierarchy for subsequent loop merges.
        // arB is inner.
        if (isInnerLoop(arB->loop, arA->loop))
            return own(new SEAddRec(add(a, arB->start), arB->step, arB->loop));
        // arA is inner.
        return own(new SEAddRec(add(arA->start, b), arA->step, arA->loop));
    }
    if (arA) return own(new SEAddRec(add(arA->start, b), arA->step, arA->loop));
    if (arB) return own(new SEAddRec(add(a, arB->start), arB->step, arB->loop));
    std::vector<SE*> ops = {a, b};
    return simplAdd(ops);
}

SE* SCEV::sub(SE* a, SE* b) {
    return add(a, neg(b));
}

SE* SCEV::mul(SE* a, int64_t f) {
    // x * 0 = 0
    if (f == 0) return own(new SEConst(0));
    // x * 1 = x
    if (f == 1) return a;
    // 3 * 4 = 12
    if (auto* c = dyn_cast<SEConst>(a)) return own(new SEConst(c->val * f));
    // (x * 2) * 3 = x * 6
    if (auto* m = dyn_cast<SEMul>(a)) return own(new SEMul(m->base, m->factor * f));
    // {s,+,st} * f = {s*f,+,st*f}
    if (auto* ar = dyn_cast<SEAddRec>(a)) return own(new SEAddRec(mul(ar->start, f), ar->step * f, ar->loop));
    if (auto* ad = dyn_cast<SEAdd>(a)) {
        std::vector<SE*> ops;
        // (a+b)*3 = a*3 + b*3
        for (auto op : ad->ops) ops.push_back(mul(op, f));
        return simplAdd(ops);
    }
    return own(new SEMul(a, f));
}

SE* SCEV::buildPhi(PhiInst* phi) {
    Loop* L = LI.loopOf(phi->getParent());
    if (!L) return own(new SEUnknown(phi));

    // Accept both simplified and rotated loops by requiring one non-loop incoming.
    Value* initVal = nullptr, *backVal = nullptr;
    int nonLoopCount = 0;
    for (int i = 0; i + 1 < phi->getNumOperands(); i += 2) {
        auto* bb = static_cast<BasicBlock*>(phi->getOperand(i + 1));
        if (!L->has(bb)) { initVal = phi->getOperand(i); ++nonLoopCount; }
        else { backVal = phi->getOperand(i); }
    }
    if (nonLoopCount != 1 || !initVal || !backVal) return own(new SEUnknown(phi));

    // Match backVal = AddRec{init, ±const}
    auto tryAddRec = [&](Instruction::OpID op, Value* lhs, Value* rhs) -> SE* {
        if (lhs != static_cast<Value*>(phi)) return nullptr;
        auto* ci = dyn_cast<ConstantInt>(rhs);
        if (!ci) return nullptr;
        int64_t step = (op == Instruction::Add) ? ci->getValue() : -ci->getValue();
        return own(new SEAddRec(get(initVal), step, L));
    };

    if (auto* bin = dyn_cast<BinaryInst>(backVal)) {
        auto op = bin->getOpID();
        if (op == Instruction::Add || op == Instruction::Sub) {
            if (auto* r = tryAddRec(op, bin->getOperand(0), bin->getOperand(1))) return r;
            if (op == Instruction::Add)
                if (auto* r = tryAddRec(op, bin->getOperand(1), bin->getOperand(0))) return r;
        }
        
        // ShiftRec: phi >>= k (Ashr) or Div->Ashr
        if (bin->getOperand(0) == static_cast<Value*>(phi)) {
            if (auto* ci = dyn_cast<ConstantInt>(bin->getOperand(1))) {
                int shiftAmt = 0;
                if (op == Instruction::Ashr)
                    shiftAmt = ci->getValue();
                else if (op == Instruction::Div) {
                    int v = ci->getValue();
                    // log2V
                    if (v > 1 && (v & (v - 1)) == 0)
                        shiftAmt = __builtin_ctz(static_cast<unsigned>(v));
                }
                if (shiftAmt > 0)
                    return own(new SEShiftRec(get(initVal), shiftAmt, L));
            }
        }
    }
    return own(new SEUnknown(phi));
}

SE* SCEV::buildGEP(GetElementPtrInst* gep) {
    auto* baseTy = static_cast<PointerType*>(gep->getOperand(0)->getType());
    Type* indexedTy = gep->getIndexedType();
    if (!indexedTy)
        indexedTy = baseTy->getPointeeType();
    int esize = elemBytes(indexedTy);
    SE* baseSE = get(gep->getOperand(0));
    SE* idxSE = get(gep->getOperand(1));
    // baseSE + idxSE * esize
    return add(baseSE, mul(idxSE, esize));
}

SE* SCEV::get(Value* v) {
    auto it = Cache.find(v);
    if (it != Cache.end()) return it->second;

    SE* result = nullptr;

    if (auto* ci = dyn_cast<ConstantInt>(v)) {
        result = own(new SEConst(ci->getValue()));
    } else if (isa<ConstantZero>(v)) {
        result = own(new SEConst(0));
    } else if (auto* phi = dyn_cast<PhiInst>(v)) {
        // Reserve cache slot first to break cycles: treat self as Unknown during build.
        Cache[v] = own(new SEUnknown(phi));
        result = buildPhi(phi);
    } else if (auto* gep = dyn_cast<GetElementPtrInst>(v)) {
        result = buildGEP(gep);
    } else if (auto* bin = dyn_cast<BinaryInst>(v)) {
        SE* lhs = get(bin->getOperand(0));
        SE* rhs = get(bin->getOperand(1));
        switch (bin->getOpID()) {
            case Instruction::Add: result = add(lhs, rhs); break;
            case Instruction::Sub: result = sub(lhs, rhs); break;
            case Instruction::Mul:
                // Mul by constant only; otherwise Unknown.
                if (auto* c = dyn_cast<SEConst>(rhs)) result = mul(lhs, c->val);
                else if (auto* c = dyn_cast<SEConst>(lhs)) result = mul(rhs, c->val);
                // Mul only supports constants on one side, because SEMul is designed to be a base * constant. 
                // If i * j are both variables, then cannot be represented by SCEV.
                else result = own(new SEUnknown(v));
                break;
            default:
                result = own(new SEUnknown(v));
        }
    } else {
        result = own(new SEUnknown(v));
    }

    // Phi, this step will overwrite the cache slot(SEUnknown) reserved above.
    Cache[v] = result;
    return result;
}

bool SCEV::nneg(SE* s) {
    if (auto* c = dyn_cast<SEConst>(s)) return c->val >= 0;
    if (auto* m = dyn_cast<SEMul>(s)) {
        if (m->factor > 0) return nneg(m->base);
        // case factor < 0: result >= 0  <->  base <= 0
        if (auto* cb = dyn_cast<SEConst>(m->base)) return cb->val <= 0;
        return false;
    }
    if (auto* ar = dyn_cast<SEAddRec>(s))
        return ar->step >= 0 && nneg(ar->start);  // non-decreasing from non-negative start
    return false;
}

bool SCEV::pos(SE* s) {
    if (auto* c = dyn_cast<SEConst>(s)) return c->val > 0;
    if (auto* m = dyn_cast<SEMul>(s)) {
        if (m->factor > 0) return pos(m->base);
        if (auto* cb = dyn_cast<SEConst>(m->base)) return cb->val < 0;
        return false;
    }
    if (auto* ar = dyn_cast<SEAddRec>(s))
        return ar->step >= 0 && pos(ar->start);
    return false;
}

// a - b != 0
bool SCEV::distinct(SE* a, SE* b) {
    SE* diff = sub(a, b);
    return pos(diff) || pos(neg(diff));
}

bool SCEV::equal(SE* a, SE* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    
    if (auto* ca = dyn_cast<SEConst>(a))
        return ca->val == cast<SEConst>(b)->val;
    if (auto* ma = dyn_cast<SEMul>(a)) {
        auto* mb = cast<SEMul>(b);
        return ma->factor == mb->factor && equal(ma->base, mb->base);
    }
    if (auto* ara = dyn_cast<SEAddRec>(a)) {
        auto* arb = cast<SEAddRec>(b);
        return ara->loop == arb->loop && ara->step == arb->step && equal(ara->start, arb->start);
    }
    if (auto* aa = dyn_cast<SEAdd>(a)) {
        auto* ab = cast<SEAdd>(b);
        if (aa->ops.size() != ab->ops.size()) return false;
        for (size_t i = 0; i < aa->ops.size(); i++)
            if (!equal(aa->ops[i], ab->ops[i])) return false;
        return true;
    }
    if (auto* sra = dyn_cast<SEShiftRec>(a)) {
        auto* srb = cast<SEShiftRec>(b);
        return sra->loop == srb->loop && sra->shiftPerIter == srb->shiftPerIter && equal(sra->start, srb->start);
    }

    return cast<SEUnknown>(a)->v == cast<SEUnknown>(b)->v;
}
