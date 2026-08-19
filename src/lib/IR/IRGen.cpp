#include "../../include/IR/IRGen.h"
#include "../../include/IR/Type.h"
#include "../../include/IR/Module.h"
#include <iostream>
#include <cassert>
#include <functional>

using namespace sysy;

static int getDimCount(Type* ty) {
    if (ty->isPointer()) return 1 + getDimCount(dyn_cast<PointerType>(ty)->getPointeeType());
    if (ty->isArray()) return 1 + getDimCount(dyn_cast<ArrayType>(ty)->getElementType());
    return 0;
}

static Type* unwrapTensorTy(Type* ty) {
    if (auto tensorTy = dyn_cast<TensorType>(ty))
        return tensorTy->getElemTy();
    return ty;
}

IRGen::IRGen() {
    TheModule = std::make_unique<Module>();
    CurrentFunc = nullptr;
}

void IRGen::defineVar(const std::string &name, Value *val) {
    if (Scopes.empty()) return;
    Scopes.back()[name] = val;
}

Value* IRGen::lookupVar(const std::string &name) {
    for (auto it = Scopes.rbegin(); it != Scopes.rend(); ++it) {
        if (it->find(name) != it->end()) return it->at(name);
    }
    return nullptr; 
}

void IRGen::defineTensor(const std::string &name, TensorInfo info) {
    if (TensorScopes.empty()) return;
    TensorScopes.back()[name] = std::move(info);
}

IRGen::TensorInfo* IRGen::lookupTensor(const std::string &name) {
    for (auto it = TensorScopes.rbegin(); it != TensorScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

Value* IRGen::castTo(Value* val, Type* targetTy) {
    targetTy = unwrapTensorTy(targetTy);
    Type* valueTy = unwrapTensorTy(val->getType());
    if (val->getType() == targetTy) return val;
    if (targetTy->isFloat() && valueTy->isInt()) {
        auto inst = builder.Create<CastInst>(Instruction::SIToFP, val, targetTy);
        inst->setName(nextValueName());
        return inst;
    }
    if (targetTy->isInt() && valueTy->isFloat()) {
        auto inst = builder.Create<CastInst>(Instruction::FPToSI, val, targetTy);
        inst->setName(nextValueName());
        return inst;
    }
    return val;
}

Value* IRGen::toCondition(Value* cond) {
    if (!cond) return nullptr;
    if (auto inst = dyn_cast<Instruction>(cond)) {
        if (inst->getOpID() == Instruction::ICmp || inst->getOpID() == Instruction::FCmp) return cond;
    }
    if (auto ci = dyn_cast<ConstantInt>(cond)) {
        auto zero = new ConstantInt(0);
        auto icmp = builder.Create<ICmpInst>(ICmpInst::NE, ci, zero);
        icmp->setName(nextValueName());
        return icmp;
    }
    if (auto cf = dyn_cast<ConstantFloat>(cond)) {
        auto zero = new ConstantFloat(0.0f);
        auto fcmp = builder.Create<FCmpInst>(FCmpInst::ONE, cf, zero);
        fcmp->setName(nextValueName());
        return fcmp;
    }
    if (cond->getType()->isFloat()) {
        auto zero = new ConstantFloat(0.0f);
        auto fcmp = builder.Create<FCmpInst>(FCmpInst::ONE, cond, zero);
        fcmp->setName(nextValueName());
        return fcmp;
    } else {
        auto zero = new ConstantInt(0);
        auto icmp = builder.Create<ICmpInst>(ICmpInst::NE, cond, zero);
        icmp->setName(nextValueName());
        return icmp;
    }
}

// Evaluate dim to get value.
int IRGen::evaluateDim(ASTNode* node) {
    if (!node) return -1;

    // 1, 2, 3
    if (auto number = dynamic_cast<NumberAST*>(node)) {
        return number->isInt() ? number->getIntVal() : 0;
    }

    // a, b, c
    if (auto lval = dynamic_cast<LValAST*>(node)) {
        Value* value = lookupVar(lval->getName());
        if (auto global = dyn_cast<GlobalVariable>(value)) {
            if (auto constant = dyn_cast<ConstantInt>(global->getInit()))
                return constant->getValue();
        }
        return 0;
    }

    if (auto unary = dynamic_cast<UnaryExprAST*>(node)) {
        int value = evaluateDim(unary->getOperand());
        if (unary->getOp() == "-") return -value;
        if (unary->getOp() == "!") return value==0;
        return value;
    }

    // a + b
    if (auto binary = dynamic_cast<BinaryExprAST*>(node)) {
        int lhs = evaluateDim(binary->getLHS());
        int rhs = evaluateDim(binary->getRHS());
        const std::string &op = binary->getOp();
        if (op == "+") return lhs + rhs;
        if (op == "-") return lhs - rhs;
        if (op == "*") return lhs * rhs;
        if (op == "/") return rhs? lhs/rhs : 0;
        if (op == "%") return rhs? lhs%rhs : 0;
        return 0;
    }

    return 0;
}

IRGen::TensorInfo IRGen::getTensorExprInfo (ExprAST *expr) {
    if (!expr) return {};

    if (auto lval = dynamic_cast<LValAST*>(expr)) {
        TensorInfo* symbol = lookupTensor(lval->getName());
        if (!symbol || lval->getIndices().size() >= symbol->Dims.size()) return {};
        TensorInfo result = *symbol;
        result.Dims.erase(result.Dims.begin(), result.Dims.begin() + lval->getIndices().size());
        result.isParameter = false;
        return result;
    }

    if (auto unary = dynamic_cast<UnaryExprAST*>(expr)) {
        if (unary->getOp() == "+" || unary->getOp() == "-")
            return getTensorExprInfo(unary->getOperand());
        return {};
    }

/// Binary
    auto binary = dynamic_cast<BinaryExprAST*>(expr);
    if (!binary) return {};
    const std::string &op = binary->getOp();
    if (op != "+" && op != "-" && op != "*" && op != "/" && op != "%" && op != "@") {
        return {};
    }

    TensorInfo lhs = getTensorExprInfo(binary->getLHS());
    TensorInfo rhs = getTensorExprInfo(binary->getRHS());

    // at evaluate
    if (op == "@") {
        // Only support 2x2 matmul.
        if (!lhs.isTensor() || !rhs.isTensor() ||
            lhs.Dims.size() != 2 || rhs.Dims.size() != 2)
            return {};
        TensorInfo result;
        result.ElementType = (lhs.ElementType->isFloat() || rhs.ElementType->isFloat()) ?
                             Type::getFloatTy() : Type::getIntTy();
        // mxn @ n*p -> result mxp
        result.Dims = {lhs.Dims[0], rhs.Dims[1]};
        return result;
    }

/// Promote scalar to tensor when lhs or rhs is Tensor, and the other is Scalar.
    if (!lhs.isTensor() && !rhs.isTensor()) return {};

    TensorInfo result = lhs.isTensor() ? lhs : rhs;
    if (lhs.isTensor() && rhs.isTensor()) {
        // Check Dims match
        if (lhs.Dims.size() != rhs.Dims.size()) return {};
        for (size_t i = 0; i < lhs.Dims.size(); i++) {
            // each Dim shoud be equal;
            if (lhs.Dims[i] >= 0 && rhs.Dims[i] >= 0 && lhs.Dims[i] != rhs.Dims[i])
                return {};
        }
    }

    // One Tensor is Float, then promote int to float.
    // here used in checking expr's TensorInfo
    if ((lhs.isTensor() && lhs.ElementType->isFloat()) || (rhs.isTensor() && rhs.ElementType->isFloat())) {
        result.ElementType = Type::getFloatTy();
    }

    // Check the number
    if (auto number = dynamic_cast<NumberAST*>(lhs.isTensor() ? binary->getRHS() : binary->getLHS())) {
        if (!number->isInt()) {
            result.ElementType = Type::getFloatTy();
        }
    }

    result.isParameter = false;
    return result;
}

Value* IRGen::getTensorElementAddress(LValAST* lval, const std::vector<int> &indices) {
    Value* address = lookupVar(lval->getName());
    TensorInfo* info = lookupTensor(lval->getName());
    if (!address || !info) return nullptr;
    if (info->isParameter) {
        auto load = builder.Create<LoadInst>(address);
        load->setName(nextValueName());
        address = load;
    } 
    else if (auto pointer = dyn_cast<PointerType>(address->getType())) {
        if (pointer->getPointeeType()->isArray()) {
            auto gep = builder.Create<GetElementPtrInst>(address, new ConstantInt(0));
            gep->setName(nextValueName());
            address = gep;
        }
    }

    for (const auto &indexExpr : lval->getIndices()) {
        indexExpr->accept(*this);
        auto gep = builder.Create<GetElementPtrInst>(address, LastVal);
        gep->setName(nextValueName());
        address = gep;
    }
    for (int index : indices) {
        auto gep = builder.Create<GetElementPtrInst>(address, new ConstantInt(index));
        gep->setName(nextValueName());
        address = gep;
    }
    return address;
}

Value* IRGen::emitTensorElement(ExprAST* expr, const std::vector<int> &indices) {
    if (auto lval = dynamic_cast<LValAST*>(expr)) {
        TensorInfo info = getTensorExprInfo(lval);
        if (info.isTensor()) {
            Value* address = getTensorElementAddress(lval, indices);
            if (!address) return nullptr;
            auto load = builder.Create<LoadInst>(address);
            load->setName(nextValueName());
            return load;
        }
        lval->accept(*this);
        return LastVal;
    }

    if (auto unary = dynamic_cast<UnaryExprAST*>(expr)) {
        TensorInfo info = getTensorExprInfo(unary);
        if (!info.isTensor()) {
            unary->accept(*this);
            return LastVal;
        }
        Value* operand = emitTensorElement(unary->getOperand(), indices);
        if (unary->getOp() == "+") return operand;
        Type* elementTy = unwrapTensorTy(operand->getType());
        Instruction::OpID op = elementTy->isFloat() ? Instruction::FSub : Instruction::Sub;
        Value* zero = elementTy->isFloat()? static_cast<Value*>(new ConstantFloat(0.0f)) 
                                          : static_cast<Value*>(new ConstantInt(0));
        auto result = builder.Create<BinaryInst>(op, zero, operand);
        result->setName(nextValueName());
        return result;
    }

    if (auto binary = dynamic_cast<BinaryExprAST*>(expr)) {
        TensorInfo resultinfo = getTensorExprInfo(binary);
        if (!resultinfo.isTensor()) {
            binary->accept(*this);
            return LastVal;
        }
        const std::string &op = binary->getOp();
        // !!!
        if (op == "@") {
            TensorInfo lhsInfo = getTensorExprInfo(binary->getLHS());
            TensorInfo rhsInfo = getTensorExprInfo(binary->getRHS());
            if (indices.size() != 2 || lhsInfo.Dims[1] != rhsInfo.Dims[0]) {
                return nullptr;
            }
            Value* sum = nullptr;
            for (int k = 0; k < lhsInfo.Dims[1]; k++) {
                Value* lhs = emitTensorElement(binary->getLHS(), {indices[0], k});
                Value* rhs = emitTensorElement(binary->getRHS(), {k, indices[1]});
                lhs = castTo(lhs, resultinfo.ElementType);
                rhs = castTo(rhs, resultinfo.ElementType);
                Instruction::OpID mulOp = resultinfo.ElementType->isFloat()? Instruction::FMul 
                                                                           : Instruction::Mul;
                auto product = builder.Create<BinaryInst>(mulOp, lhs, rhs);
                product->setName(nextValueName());
                if (!sum) sum = product;
                else {
                    Instruction::OpID addOp = resultinfo.ElementType->isFloat()? Instruction::FAdd : Instruction::Add;
                    auto add = builder.Create<BinaryInst>(addOp, sum, product);
                    add->setName(nextValueName());
                    sum = add;
                }          
            }
            return sum;
        }

        TensorInfo lhsInfo = getTensorExprInfo(binary->getLHS());
        TensorInfo rhsInfo = getTensorExprInfo(binary->getRHS());
        Value* lhs = nullptr;
        if (lhsInfo.isTensor()) {
            lhs = emitTensorElement(binary->getLHS(), indices);
        } else {
            binary->getLHS()->accept(*this);
            lhs = LastVal;
        }
        Value* rhs = nullptr;
        if (rhsInfo.isTensor()) {
            rhs = emitTensorElement(binary->getRHS(), indices);
        } else {
            binary->getRHS()->accept(*this);
            rhs = LastVal;
        }
        Type* elementTy = lhs->getType()->isFloat()? Type::getFloatTy() : Type::getIntTy()
                          || rhs->getType()->isFloat()? Type::getFloatTy() : Type::getIntTy();
        if (op == "%" && elementTy->isFloat()) return nullptr;

        // promote
        lhs = castTo(lhs, elementTy);
        rhs = castTo(rhs, elementTy);
        Instruction::OpID opID = Instruction::Add;
        if (elementTy->isFloat()) {
            if (op == "+") opID = Instruction::FAdd;
            else if (op == "-") opID = Instruction::FSub;
            else if (op == "*") opID = Instruction::FMul;
            else if (op == "/") opID = Instruction::FDiv;
        } else {
            if (op == "+") opID = Instruction::Add;
            if (op == "-") opID = Instruction::Sub;
            if (op == "*") opID = Instruction::Mul;
            if (op == "/") opID = Instruction::Div;
            if (op == "%") opID = Instruction::Mod;
        }
        auto result = builder.Create<BinaryInst>(opID, lhs, rhs);
        result->setName(nextValueName());
        return result;
    }

    expr->accept(*this);
    return LastVal;
}

bool IRGen::lowerTensorAssignment(AssignStmtAST &node) {
    TensorInfo target = getTensorExprInfo(node.getLVal());
    if (!target.isTensor()) return false;
    TensorInfo value = getTensorExprInfo(node.getValue());
    if (!value.isTensor()) 
        return true;
    if (target.Dims.size() != value.Dims.size()) 
        return true;

    std::vector<int> shape = target.Dims;
    for (size_t i = 0; i < shape.size(); i++) {
        if (shape[i] < 0) 
            shape[i] = value.Dims[i];
        if (value.Dims[i] >= 0 && shape[i] != value.Dims[i]) 
            return true;
    }

    size_t elementCnt = 1;
    for (int dim : shape) {
        if (dim <= 0) return true;
        elementCnt *= static_cast<size_t>(dim);
    }
    std::vector<std::vector<int>> allIndices;
    std::vector<Value*> allElements;
    allIndices.reserve(elementCnt);
    allElements.reserve(elementCnt);
    for (size_t flat = 0; flat < elementCnt; flat++) {
        size_t remainder = flat;
        std::vector<int> indices(shape.size());
        for (size_t i = shape.size(); i > 0; i--) {
            indices[i-1] = static_cast<int>(remainder % shape[i-1]);
            remainder /= shape[i-1];
        }
        Value* element = emitTensorElement(node.getValue(), indices);
        if (!element) return true;
        allIndices.push_back(std::move(indices));
        allElements.push_back(castTo(element, target.ElementType));
    }

    for (size_t i = 0; i < allElements.size(); i++) {
        Value* address = getTensorElementAddress(node.getLVal(), allIndices[i]);
        if (!address) return true;
        builder.Create<StoreInst>(allElements[i], address);
    }
    LastVal = nullptr;
    return true;
}


Constant* IRGen::evaluateConstantExpr(Value* val) {
    if (!val) return nullptr;
    if (auto c = dyn_cast<Constant>(val)) return c;

    if (auto inst = dyn_cast<Instruction>(val)) {
        if (inst->getOpID() == Instruction::Load) {
            if (auto gv = dyn_cast<GlobalVariable>(inst->getOperand(0))) {
                if (gv->isConst())
                    return evaluateConstantExpr(gv->getInit());
            }
        }
    }

    if (auto bin = dyn_cast<BinaryInst>(val)) {
        auto lhs = evaluateConstantExpr(bin->getOperand(0));
        auto rhs = evaluateConstantExpr(bin->getOperand(1));
        if (lhs && rhs) {
            if (auto i1 = dyn_cast<ConstantInt>(lhs)) {
                if (auto i2 = dyn_cast<ConstantInt>(rhs)) {
                    int v1 = i1->getValue(), v2 = i2->getValue();
                    switch (bin->getOpID()) {
                        case Instruction::Add: return new ConstantInt(v1 + v2);
                        case Instruction::Sub: return new ConstantInt(v1 - v2);
                        case Instruction::Mul: return new ConstantInt(v1 * v2);
                        case Instruction::Div: return v2 != 0 ? new ConstantInt(v1 / v2) : nullptr;
                        case Instruction::Mod: return v2 != 0 ? new ConstantInt(v1 % v2) : nullptr;
                        default: return nullptr;
                    }
                }
            } else if (auto f1 = dyn_cast<ConstantFloat>(lhs)) {
                if (auto f2 = dyn_cast<ConstantFloat>(rhs)) {
                    float v1 = f1->getValue(), v2 = f2->getValue();
                    switch (bin->getOpID()) {
                        case Instruction::FAdd: return new ConstantFloat(v1 + v2);
                        case Instruction::FSub: return new ConstantFloat(v1 - v2);
                        case Instruction::FMul: return new ConstantFloat(v1 * v2);
                        case Instruction::FDiv: return v2 != 0.0f ? new ConstantFloat(v1 / v2) : nullptr;
                        default: return nullptr;
                    }
                }
            }
        }
    } else if (auto castInst = dyn_cast<CastInst>(val)) {
        auto op = evaluateConstantExpr(castInst->getOperand(0));
        if (op) {
            if (castInst->getOpID() == Instruction::SIToFP) {
                if (auto c = dyn_cast<ConstantInt>(op)) return new ConstantFloat((float)c->getValue());
            } else if (castInst->getOpID() == Instruction::FPToSI) {
                if (auto c = dyn_cast<ConstantFloat>(op)) return new ConstantInt((int)c->getValue());
            }
        }
    }
    return nullptr;
}

Constant* IRGen::getGlobalInitVal(InitValAST* init, Type* type) {
    std::function<int(Type*)> getFlattenedSize = [&](Type* ty) -> int {
        if (auto arrTy = dyn_cast<ArrayType>(ty)) {
            return arrTy->getNumElements() * getFlattenedSize(arrTy->getElementType());
        }
        return 1;
    };

    int totalSize = getFlattenedSize(type);
    std::vector<Constant*> flat_vals(totalSize, nullptr);
    int flat_idx = 0;

    std::function<void(InitValAST*, Type*, int&)> flatten = [&](InitValAST* node, Type* ty, int& idx) {
        if (node->isLeaf()) {
            node->getExpr()->accept(*this);
            Value* computedVal = LastVal;
            if (auto evaluated = evaluateConstantExpr(computedVal)) computedVal = evaluated;
            
            Type* basicTy = ty;
            while (auto arrTy = dyn_cast<ArrayType>(basicTy)) basicTy = arrTy->getElementType();
            basicTy = unwrapTensorTy(basicTy);
            Constant* cval = dyn_cast<Constant>(computedVal);
            if (!cval) cval = basicTy->isFloat() ? (Constant*)new ConstantFloat(0.0f) : (Constant*)new ConstantInt(0);
            else {
                if (basicTy->isFloat() && cval->getType()->isInt()) {
                    cval = new ConstantFloat((float)cast<ConstantInt>(cval)->getValue());
                } else if (basicTy->isInt() && cval->getType()->isFloat()) {
                    cval = new ConstantInt((int)cast<ConstantFloat>(cval)->getValue());
                }
            }
            if (idx < totalSize) flat_vals[idx++] = cval;
            return;
        }
        if (auto arrTy = dyn_cast<ArrayType>(ty)) {
            Type* elemTy = arrTy->getElementType();
            int elemCount = arrTy->getNumElements();
            int subSize = getFlattenedSize(elemTy);
            const auto& elems = node->getElements();
            int start_idx = idx;
            for (size_t i = 0; i < elems.size(); ++i) {
                if (!elems[i]->isLeaf()) {
                    int cur_off = idx - start_idx;
                    if (cur_off % subSize != 0) idx += subSize - (cur_off % subSize);
                }
                flatten(elems[i].get(), elemTy, idx);
            }
            int cur_off = idx - start_idx;
            int total = elemCount * subSize;
            if (cur_off < total) idx += total - cur_off;
        }
    };

    flatten(init, type, flat_idx);

    std::function<Constant*(Type*, int&)> buildArray = [&](Type* ty, int& idx) -> Constant* {
        if (auto arrTy = dyn_cast<ArrayType>(ty)) {
            std::vector<Constant*> elems;
            for (int i = 0; i < arrTy->getNumElements(); ++i) {
                elems.push_back(buildArray(arrTy->getElementType(), idx));
            }
            return new ConstantArray(arrTy, elems);
        } else {
            ty = unwrapTensorTy(ty);
            Constant* val = flat_vals[idx++];
            if (!val) val = ty->isFloat() ? (Constant*)new ConstantFloat(0.0f) : (Constant*)new ConstantInt(0);
            return val;
        }
    };

    int build_idx = 0;
    return buildArray(type, build_idx);
}

void IRGen::processLocalInit(InitValAST* init, Value* baseAddr, Type* type, std::vector<int>&) {
    std::function<int(Type*)> getFlattenedSize = [&](Type* ty) -> int {
        if (auto arrTy = dyn_cast<ArrayType>(ty)) return arrTy->getNumElements() * getFlattenedSize(arrTy->getElementType());
        return 1;
    };

    int totalSize = getFlattenedSize(type);
    std::vector<Value*> flat_vals(totalSize, nullptr);
    int flat_idx = 0;

    std::function<void(InitValAST*, Type*, int&)> flatten = [&](InitValAST* node, Type* ty, int& idx) {
        if (node->isLeaf()) {
            node->getExpr()->accept(*this);
            Value* val = LastVal;
            Type* basicTy = ty;
            while (auto arrTy = dyn_cast<ArrayType>(basicTy)) basicTy = arrTy->getElementType();
            basicTy = unwrapTensorTy(basicTy);
            val = castTo(val, basicTy);
            if (idx < totalSize) flat_vals[idx++] = val;
            return;
        }
        if (auto arrTy = dyn_cast<ArrayType>(ty)) {
            Type* elemTy = arrTy->getElementType();
            int elemCount = arrTy->getNumElements();
            int subSize = getFlattenedSize(elemTy);
            const auto& elems = node->getElements();
            int start_idx = idx;
            for (size_t i = 0; i < elems.size(); ++i) {
                if (!elems[i]->isLeaf()) {
                    int cur_off = idx - start_idx;
                    if (cur_off % subSize != 0) idx += subSize - (cur_off % subSize);
                }
                flatten(elems[i].get(), elemTy, idx);
            }
            int cur_off = idx - start_idx;
            int total = elemCount * subSize;
            if (cur_off < total) idx += total - cur_off;
        }
    };

    flatten(init, type, flat_idx);

    Value* ptr = baseAddr;
    Type* basicTy = type;
    while (auto pTy = dyn_cast<ArrayType>(cast<PointerType>(ptr->getType())->getPointeeType())) {
        auto zero = new ConstantInt(0);
        ptr = builder.Create<GetElementPtrInst>(ptr, zero);
        ptr->setName(nextValueName());
        basicTy = pTy->getElementType();
    }
    basicTy = unwrapTensorTy(basicTy);
    Constant* constZero = basicTy->isFloat() ? (Constant*)new ConstantFloat(0.0f) : (Constant*)new ConstantInt(0);
    
    if (!type->isArray()) {
        Value* val = flat_vals[0] ? flat_vals[0] : constZero;
        builder.Create<StoreInst>(val, ptr);
        return;
    }

    bool isAllZero = true;
    for (int i = 0; i < totalSize; ++i) {
        if (flat_vals[i] != nullptr && !isa<ConstantZero>(flat_vals[i])) {
            if (auto ci = dyn_cast<ConstantInt>(flat_vals[i])) { if (ci->getValue() != 0) isAllZero = false; }
            else if (auto cf = dyn_cast<ConstantFloat>(flat_vals[i])) { if (cf->getValue() != 0.0) isAllZero = false; }
            else isAllZero = false;
        }
        if (!isAllZero) break;
    }

    if (isAllZero && totalSize >= 8) {
        BasicBlock* condBB = new BasicBlock(newLabelName(), CurrentFunc->getBody());
        BasicBlock* bodyBB = new BasicBlock(newLabelName(), CurrentFunc->getBody());
        BasicBlock* endBB = new BasicBlock(newLabelName(), CurrentFunc->getBody());

        // Hoist loop counter/pointer allocas to entry block so Mem2Reg can promote them.
        // If they stay in the current (non-entry) block, FlattenCFG may reorder the
        // init-loop BBs before the setup BB, causing InstSel to see uses before the def.
        BasicBlock* entryBB = CurrentFunc->getEntryBlock();
        auto& instList = entryBB->getInstructions();
        assert(!instList.empty());
        // fix at least two localarray init, so delete the assert.  
        auto insertAllocaInEntry = [&](AllocaInst* ai) {
            instList.pop_back(); // Remove the AllocaInst just auto-appended by constructor.
            auto it = instList.begin();
            while (it != instList.end() && (*it)->getOpID() == Instruction::Alloca) ++it;
            instList.insert(it, ai);
        };

        // idx
        auto idxAlloca = new AllocaInst(Type::getIntTy(), entryBB);
        insertAllocaInEntry(idxAlloca);
        builder.Create<StoreInst>(new ConstantInt(0), idxAlloca);
        // curPtr
        auto ptrAlloca = new AllocaInst(ptr->getType(), entryBB);
        insertAllocaInEntry(ptrAlloca);
        builder.Create<StoreInst>(ptr, ptrAlloca);

        builder.Create<BranchInst>(condBB);

        // Cond: while (i < totalSize)
        builder.SetInsertPoint(condBB);
        auto iLoad = builder.Create<LoadInst>(idxAlloca);
        iLoad->setName(nextValueName());
        auto cmp = builder.Create<ICmpInst>(ICmpInst::SLT, iLoad, new ConstantInt(totalSize));
        cmp->setName(nextValueName());
        builder.Create<BranchInst>(cmp, bodyBB, endBB);

        // Body: *curPtr = 0; curPtr++; idx++;
        builder.SetInsertPoint(bodyBB);
        auto currentPtr = builder.Create<LoadInst>(ptrAlloca);
        currentPtr->setName(nextValueName());
        builder.Create<StoreInst>(constZero, currentPtr);

        auto nextPtr = builder.Create<GetElementPtrInst>(currentPtr, new ConstantInt(1));
        nextPtr->setName(nextValueName());
        builder.Create<StoreInst>(nextPtr, ptrAlloca);

        auto iNext = builder.Create<BinaryInst>(Instruction::Add, iLoad, new ConstantInt(1));
        iNext->setName(nextValueName());
        builder.Create<StoreInst>(iNext, idxAlloca);

        builder.Create<BranchInst>(condBB);

        // End
        builder.SetInsertPoint(endBB);
        return;
    }
    
    for (int i = 0; i < totalSize; ++i) {
        Value* val = flat_vals[i] ? flat_vals[i] : constZero;
        auto gep = builder.Create<GetElementPtrInst>(ptr, new ConstantInt(i));
        gep->setName(nextValueName());
        builder.Create<StoreInst>(val, gep);
    }
}

void IRGen::fillZero(Value*, Type*, std::vector<int>&) { }

void IRGen::visit(CompUnitAST &node) {
    enterScope();
    for (auto &child : node.getChildren()) child->accept(*this);
    exitScope();
}

void IRGen::visit(FuncCallAST &node) {
    std::string funcName = node.getName();
    if (funcName == "starttime") funcName = "_sysy_starttime";
    else if (funcName == "stoptime") funcName = "_sysy_stoptime";

    Function *callee = TheModule->getFunction(funcName);
    if (!callee) {
        Type* retType = Type::getIntTy(); 
        if (funcName == "putint" || funcName == "putch" || funcName == "putarray" ||
            funcName == "putfarray" || funcName == "putfloat" || funcName == "_sysy_starttime" || funcName == "_sysy_stoptime")
            retType = Type::getVoidTy();
        else if (funcName == "getint" || funcName == "getch" || funcName == "getarray" || funcName == "getfarray")
            retType = Type::getIntTy();
        else if (funcName == "getfloat")
            retType = Type::getFloatTy();
            
        callee = new Function(funcName, retType);
        if (funcName == "_sysy_starttime" || funcName == "_sysy_stoptime") {
            callee->addArgument(new Argument(Type::getIntTy(), "lineno", callee, 0));
        }
        TheModule->addFunction(callee);
    }

    std::vector<Value*> args;
    if (funcName == "_sysy_starttime" || funcName == "_sysy_stoptime") {
        args.push_back(new ConstantInt(0));
    } else {
        int argIdx = 0;
        for (auto &argNode : node.getArgs()) {
            bool oldMode = isLValMode;
            isLValMode = false;
            argNode->accept(*this);
            isLValMode = oldMode;
            
            Value* argVal = LastVal;
            Type* expectedTy = nullptr;
            
            if ((int)callee->getArgs().size() > argIdx) {
                expectedTy = callee->getArgs()[argIdx]->getType();
            } else {
                if (funcName == "putint" || funcName == "putch") {
                    expectedTy = Type::getIntTy();
                } else if (funcName == "putfloat") {
                    expectedTy = Type::getFloatTy();
                }
            }
            
            if (expectedTy) {
                argVal = castTo(argVal, expectedTy);
            }
            
            args.push_back(argVal);
            argIdx++;
        }
    }

    auto call = builder.Create<CallInst>(callee, args);
    if (!callee->getType()->isVoid()) {
        call->setName(nextValueName());
        LastVal = call;
    } else {
        LastVal = nullptr;
    }
}

void IRGen::visit(FuncDefAST &node) {
    Type* retType = Type::getIntTy();
    if (node.getRetType() == "void") retType = Type::getVoidTy();
    else if (node.getRetType() == "float") retType = Type::getFloatTy();
    else if (node.returnTensor()) {
        Type* elemTy = node.getRetType() == "tensor float" ? Type::getFloatTy() : Type::getIntTy();
        retType = new TensorType(elemTy);
    }
    auto func = new Function(node.getName(), retType);
    TheModule->addFunction(func);
    CurrentFunc = func;
    ValueCounter = 0;
    LabelCounter = 0;

    for (size_t i = 0; i < node.getParams().size(); ++i) {
        auto &paramNode = node.getParams()[i];
        Type* baseTy = (paramNode->getElementType() == "float") ? Type::getFloatTy() : Type::getIntTy();
        if (paramNode->isTensor()) baseTy = new TensorType(baseTy);
        Type* argTy = baseTy;

        if (!paramNode->getDims().empty()) {
            std::function<int(ASTNode*)> evalConst = [&](ASTNode* n) -> int {
                if (!n) return 0;
                if (auto num = dynamic_cast<NumberAST*>(n)) return num->getIntVal();
                if (auto lval = dynamic_cast<LValAST*>(n)) {
                    Value* v = lookupVar(lval->getName());
                    if (v) {
                        if (auto gv = dyn_cast<GlobalVariable>(v)) {
                            if (auto ci = dyn_cast<ConstantInt>(gv->getInit())) {
                                return ci->getValue();
                            }
                        }
                    }
                    return 0; 
                }
                if (auto bin = dynamic_cast<BinaryExprAST*>(n)) {
                    int lhs = evalConst(bin->getLHS());
                    int rhs = evalConst(bin->getRHS());
                    std::string op = bin->getOp();
                    if (op == "+") return lhs + rhs;
                    if (op == "-") return lhs - rhs;
                    if (op == "*") return lhs * rhs;
                    if (op == "/") return rhs != 0 ? lhs / rhs : 0;
                    if (op == "%") return rhs != 0 ? lhs % rhs : 0;
                    return 0;
                }
                if (auto un = dynamic_cast<UnaryExprAST*>(n)) {
                    int val = evalConst(un->getOperand());
                    if (un->getOp() == "-") return -val;
                    if (un->getOp() == "!") return val == 0 ? 1 : 0;
                    return val;
                }
                return 0;
            };

            const auto &dims = paramNode->getDims();
            for (auto it = dims.size(); it > 1; it--) {
                size_t dimIndex = it - 1;
                int size = evalConst(dims[dimIndex].get());
                baseTy = new ArrayType(baseTy, size);
            }
            argTy = new PointerType(baseTy);
        }
        std::string argName = "%arg" + std::to_string(i);
        auto arg = new Argument(argTy, argName, func, i);
        func->addArgument(arg);
    }

    BasicBlock *entryBlock = new BasicBlock(newLabelName(), func->getBody());
    builder.SetInsertPoint(entryBlock);

    enterScope();

    const auto &args = func->getArgs();
    for (size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        auto &paramNode = node.getParams()[i];

        auto alloca = builder.Create<AllocaInst>(arg->getType());
        alloca->setName("%" + paramNode->getName() + "_addr");
        builder.Create<StoreInst>(arg, alloca);
        defineVar(paramNode->getName(), alloca);
        if (paramNode->isTensor()) {
            TensorInfo info;
            info.ElementType = paramNode->getElementType() == "float" ? Type::getFloatTy() : Type::getIntTy();
            info.isParameter = true;
            for (const auto &dim :paramNode->getDims())
                info.Dims.push_back(dim? evaluateDim(dim.get()) : -1);
            defineTensor(paramNode->getName(), std::move(info));
        }
    }

    if (node.getBody()) node.getBody()->accept(*this);
    
    exitScope();

    BasicBlock *curr = builder.GetInsertPoint();
    if (curr->getInstructions().empty() || !curr->getInstructions().back()->isTerminator()) {
        if (retType->isVoid()) builder.CreateRet(nullptr);
        else builder.CreateRet(new ConstantInt(0));
    }
    CurrentFunc = nullptr;
}

void IRGen::visit(FuncFParamAST&) {}

void IRGen::visit(BlockAST &node) {
    enterScope();
    for (auto &item : node.getItems()) {
        item->accept(*this);
        if (builder.GetInsertPoint()->getInstructions().size() > 0 &&
            builder.GetInsertPoint()->getInstructions().back()->isTerminator()) {
            break;
        }
    }
    exitScope();
}

void IRGen::visit(VarDeclAST &node) {
    Type* varType = (node.getElementType() == "float") ? Type::getFloatTy() : Type::getIntTy();
    if (node.isTensor())
        varType = new TensorType(varType);
    std::function<int(ASTNode*)> evalConst = [&](ASTNode* n) -> int {
        if (!n) return 0;
        if (auto num = dynamic_cast<NumberAST*>(n)) return num->getIntVal();
        if (auto lval = dynamic_cast<LValAST*>(n)) {
                    Value* v = lookupVar(lval->getName());
                    if (v) {
                        if (auto gv = dyn_cast<GlobalVariable>(v)) {
                            if (auto ci = dyn_cast<ConstantInt>(gv->getInit())) {
                                return ci->getValue();
                            }
                        }
                    }
                    return 0; 
                }
        if (auto bin = dynamic_cast<BinaryExprAST*>(n)) {
            int lhs = evalConst(bin->getLHS());
            int rhs = evalConst(bin->getRHS());
            std::string op = bin->getOp();
            if (op == "+") return lhs + rhs;
            if (op == "-") return lhs - rhs;
            if (op == "*") return lhs * rhs;
            if (op == "/") return rhs != 0 ? lhs / rhs : 0;
            if (op == "%") return rhs != 0 ? lhs % rhs : 0;
            return 0;
        }
        if (auto un = dynamic_cast<UnaryExprAST*>(n)) {
            int val = evalConst(un->getOperand());
            if (un->getOp() == "-") return -val;
            if (un->getOp() == "!") return val == 0 ? 1 : 0;
            return val;
        }
        return 0;
    };

    const auto &dims = node.getDims();
    for (auto it = dims.size(); it > 0; it--) {
        size_t dimIndex = it - 1;
        int size = evalConst(dims[dimIndex].get());
        varType = new ArrayType(varType, size);
    }

    if (CurrentFunc == nullptr) {
        Constant *initVal = nullptr;
        if (node.getInit()) initVal = getGlobalInitVal(node.getInit(), varType);
        else initVal = new ConstantZero(varType);
        
        auto globalVar = new GlobalVariable(node.getName(), varType, initVal);
        globalVar->setConst(node.isConst());
        TheModule->addGlobalVariable(globalVar);
        defineVar(node.getName(), globalVar);
        if (node.isTensor()) {
            TensorInfo info;
            info.ElementType = node.getElementType() == "float" ? Type::getFloatTy() : Type::getIntTy();
            for (const auto &dim : dims)
                info.Dims.push_back(evaluateDim(dim.get()));
            defineTensor(node.getName(), std::move(info));
        }
        return;
    }

    BasicBlock* entryBB = CurrentFunc->getEntryBlock();
    auto alloca = new AllocaInst(varType, entryBB);
    auto& instList = entryBB->getInstructions();
    instList.pop_back();

    auto it = instList.begin();
    while (it != instList.end() && (*it)->getOpID() == Instruction::Alloca) ++it;
    instList.insert(it, alloca);

    alloca->setName("%" + node.getName() + "_" + std::to_string(ValueCounter++));
    defineVar(node.getName(), alloca);
    if (node.isTensor()) {
        TensorInfo info;
        info.ElementType = node.getElementType() == "float" ? Type::getFloatTy() : Type::getIntTy();
        for (const auto &dim : dims)
            info.Dims.push_back(evaluateDim(dim.get()));
        defineTensor(node.getName(), std::move(info));
    }

    if (node.getInit()) {
        std::vector<int> indices;
        processLocalInit(node.getInit(), alloca, varType, indices);
    }
}

void IRGen::visit(BreakStmtAST&) { builder.Create<BreakInst>(); }

void IRGen::visit(ContinueStmtAST&) { builder.Create<ContinueInst>(); }

void IRGen::visit(AssignStmtAST &node) {
    if (lowerTensorAssignment(node)) return;
    if (getTensorExprInfo(node.getValue()).isTensor()) return;
    isLValMode = true;
    node.getLVal()->accept(*this);
    isLValMode = false;
    Value *addr = LastVal;

    node.getValue()->accept(*this);
    Value *val = LastVal;

    if (addr && val) {
        Type* targetTy = dyn_cast<PointerType>(addr->getType())->getPointeeType();
        val = castTo(val, targetTy);
        builder.Create<StoreInst>(val, addr);
    }
}

void IRGen::visit(LValAST &node) {
    Value *addr = lookupVar(node.getName());
    if (!addr) { LastVal = nullptr; return; }

    int maxIndices = getDimCount(addr->getType()) - 1;
    bool isParamPointer = false;

    if (auto ptrTy = dyn_cast<PointerType>(addr->getType())) {
        if (ptrTy->getPointeeType()->isPointer()) {
            auto load = builder.Create<LoadInst>(addr);
            load->setName(nextValueName());
            addr = load;
            isParamPointer = true;
        }
    }

    if (!isParamPointer) {
        if (auto ptrTy = dyn_cast<PointerType>(addr->getType())) {
            if (ptrTy->getPointeeType()->isArray()) {
                auto zero = new ConstantInt(0);
                auto gep = builder.Create<GetElementPtrInst>(addr, zero);
                gep->setName(nextValueName());
                addr = gep;
            }
        }
    }

    bool isPartial = (int)node.getIndices().size() < maxIndices;

    for (auto &indexExpr : node.getIndices()) {
        bool oldMode = isLValMode;
        isLValMode = false;
        indexExpr->accept(*this);
        isLValMode = oldMode;
        Value *indexVal = LastVal;

        auto gep = builder.Create<GetElementPtrInst>(addr, indexVal);
        gep->setName(nextValueName());
        addr = gep;
    }

    // Return the current address/pointer directly if no full index to a scalar element 
    if (isLValMode || isPartial) {
        LastVal = addr;
    } else {
        auto load = builder.Create<LoadInst>(addr);
        load->setName(nextValueName());
        LastVal = load;
    }
}

void IRGen::visit(NumberAST &node) {
    if (node.isInt()) LastVal = new ConstantInt(node.getIntVal()); 
    else LastVal = new ConstantFloat(node.getFloatVal());
}

void IRGen::visit(BinaryExprAST &node) {
    std::string opStr =node.getOp();
    if (opStr == "&&" || opStr == "||") {
        node.getLHS()->accept(*this);
        Value *L = toCondition(LastVal);

        if (auto ci = dyn_cast<ConstantInt>(L)) {
            if (opStr == "&&") {
                if (ci->getValue() == 0) {
                    LastVal = new ConstantInt(0); // false && x is false.
                    return;
                } else {
                    node.getRHS()->accept(*this); // true && x is x.
                    LastVal = toCondition(LastVal);
                    return;
                }
            } else if (opStr == "||") {
                if (ci->getValue() == 1) {
                    LastVal = new ConstantInt(1); // true || x is true.
                    return;
                } else {
                    node.getRHS()->accept(*this); // false || x is x.
                    LastVal = toCondition(LastVal);
                    return;
                }
            }
        }

        BasicBlock* currentBB = builder.GetInsertPoint();
        Region* currentRegion = currentBB->getParent();
        BasicBlock* rhsBB = new BasicBlock(newLabelName(), currentRegion);
        BasicBlock* mergeBB = new BasicBlock(newLabelName(), currentRegion);

        if (opStr == "&&") builder.Create<BranchInst>(L, rhsBB, mergeBB);
        else builder.Create<BranchInst>(L, mergeBB, rhsBB);

        builder.SetInsertPoint(rhsBB);
        node.getRHS()->accept(*this);
        Value* R = toCondition(LastVal);
        BasicBlock* rhsEndBB = builder.GetInsertPoint(); 
        builder.Create<BranchInst>(mergeBB);

        builder.SetInsertPoint(mergeBB);
        auto phi = builder.Create<PhiInst>(Type::getIntTy());
        phi->setName(nextValueName());

        if (opStr == "&&") {
            phi->addIncoming(new ConstantInt(0), currentBB);
            phi->addIncoming(R, rhsEndBB);
        } else {
            phi->addIncoming(new ConstantInt(1), currentBB);
            phi->addIncoming(R, rhsEndBB);
        }

        LastVal = phi;
        return;
    }

    node.getLHS()->accept(*this); Value *L = LastVal;
    node.getRHS()->accept(*this); Value *R = LastVal;
    if (!L || !R) return;

    bool isFloat = L->getType()->isFloat() || R->getType()->isFloat();
    Type* targetTy = isFloat ? Type::getFloatTy() : Type::getIntTy();
    L = castTo(L, targetTy); R = castTo(R, targetTy);

    Instruction *inst = nullptr;
    if (opStr == ">" || opStr == "<" || opStr == "==" || opStr == ">=" || opStr == "<=" || opStr == "!=") {
        if (isFloat) {
            FCmpInst::CmpOp pred = FCmpInst::OEQ;
            if (opStr == ">") pred = FCmpInst::OGT;
            else if (opStr == "<") pred = FCmpInst::OLT;
            else if (opStr == "==") pred = FCmpInst::OEQ;
            else if (opStr == "!=") pred = FCmpInst::ONE;
            else if (opStr == ">=") pred = FCmpInst::OGE;
            else if (opStr == "<=") pred = FCmpInst::OLE;
            inst = builder.Create<FCmpInst>(pred, L, R);
        } else {
            ICmpInst::CmpOp pred = ICmpInst::EQ;
            if (opStr == ">") pred = ICmpInst::SGT;
            else if (opStr == "<") pred = ICmpInst::SLT;
            else if (opStr == "==") pred = ICmpInst::EQ;
            else if (opStr == "!=") pred = ICmpInst::NE;
            else if (opStr == ">=") pred = ICmpInst::SGE;
            else if (opStr == "<=") pred = ICmpInst::SLE;
            inst = builder.Create<ICmpInst>(pred, L, R);
        }
    } else {
        Instruction::OpID op = Instruction::Add;
        if (isFloat) {
            if (opStr == "+") op = Instruction::FAdd;
            else if (opStr == "-") op = Instruction::FSub;
            else if (opStr == "*") op = Instruction::FMul;
            else if (opStr == "/") op = Instruction::FDiv;
        } else {
            if (opStr == "+") op = Instruction::Add;
            else if (opStr == "-") op = Instruction::Sub;
            else if (opStr == "*") op = Instruction::Mul;
            else if (opStr == "/") op = Instruction::Div;
            else if (opStr == "%") op = Instruction::Mod;
        }
        inst = builder.Create<BinaryInst>(op, L, R);
    }
    if (inst) {
        inst->setName(nextValueName());
        LastVal = inst;
    }
}

void IRGen::visit(IfStmtAST &node) {
    node.getCond()->accept(*this);
    Value *cond = toCondition(LastVal);
    if (!cond) return;

    auto ifInst = builder.Create<IfInst>(cond);
    {
        BasicBlock *thenBlock = new BasicBlock(newLabelName(), ifInst->getThenRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        builder.SetInsertPoint(thenBlock);
        if (node.getThen()) node.getThen()->accept(*this);
        builder.SetInsertPoint(originalBlock);
    }
    if (node.getElse()) {
        ifInst->addElseRegion();
        BasicBlock *elseBlock = new BasicBlock(newLabelName(), ifInst->getElseRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        builder.SetInsertPoint(elseBlock);
        node.getElse()->accept(*this);
        builder.SetInsertPoint(originalBlock);
    }
}

void IRGen::visit(WhileStmtAST &node) {
    auto whileInst = builder.Create<WhileInst>();
    {
        BasicBlock *condBlock = new BasicBlock(newLabelName(), whileInst->getCondRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        builder.SetInsertPoint(condBlock);
        node.getCond()->accept(*this);
        LastVal = toCondition(LastVal);
        builder.SetInsertPoint(originalBlock);
    }
    {
        BasicBlock *bodyBlock = new BasicBlock(newLabelName(), whileInst->getBodyRegion());
        BasicBlock *originalBlock = builder.GetInsertPoint();
        builder.SetInsertPoint(bodyBlock);
        if (node.getBody()) node.getBody()->accept(*this);
        builder.SetInsertPoint(originalBlock);
    }
}

void IRGen::visit(ReturnStmtAST &node) {
    if (node.getRetVal()) {
        node.getRetVal()->accept(*this);
        Value *retVal = castTo(LastVal, CurrentFunc->getType());
        builder.CreateRet(retVal);
    } else {
        if (CurrentFunc->getType()->isVoid()) builder.CreateRet(nullptr);
        else builder.CreateRet(new ConstantZero(CurrentFunc->getType()));
    }
}

void IRGen::visit(ExprStmtAST &node) {
    if (node.getExpr()) node.getExpr()->accept(*this);
}

void IRGen::visit(UnaryExprAST &node) {
    node.getOperand()->accept(*this);
    Value *operand = LastVal;
    if (node.getOp() == "-") {
        Instruction* inst = nullptr;
        if (operand->getType()->isFloat()) {
            auto zero = new ConstantFloat(0.0f);
            inst = builder.Create<BinaryInst>(Instruction::FSub, zero, operand);
        } else {
            auto zero = new ConstantInt(0);
            inst = builder.Create<BinaryInst>(Instruction::Sub, zero, operand);
        }
        inst->setName(nextValueName());
        LastVal = inst;
    }
    else if (node.getOp() == "!") {
        operand = toCondition(operand);
        auto zero = new ConstantInt(0);
        auto inst = builder.Create<ICmpInst>(ICmpInst::EQ, operand, zero);
        inst->setName(nextValueName());
        LastVal = inst;
    }
    else if (node.getOp() == "+") {}
}
