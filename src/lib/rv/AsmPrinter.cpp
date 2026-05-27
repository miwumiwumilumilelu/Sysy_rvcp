#include "rv/AsmPrinter.h"
#include "rv/RvOp.h"
#include "IR/Value.h"
#include "IR/Type.h"
#include <vector>
#include <cstdint>
#include <cstring>

namespace sysy {
namespace rv {

static bool isAllZero(Constant* c) {
    if (!c) return true;
    if (isa<ConstantZero>(c)) return true;
    if (isa<ConstantInt>(c)) return cast<ConstantInt>(c)->getValue() == 0;
    if (isa<ConstantFloat>(c)) return cast<ConstantFloat>(c)->getValue() == 0.0f;
    if (isa<ConstantArray>(c)) {
        for (auto* e : cast<ConstantArray>(c)->getConsts())
            if (!isAllZero(e)) return false;
        return true;
    }
    return false;
}

// Get muldims array size.
static int totalLeafCount(Type* ty) {
    if (ty->isArray()) {
        auto* at = cast<ArrayType>(ty);
        return at->getNumElements() * totalLeafCount(at->getElementType());
    }
    return 1;
}

// Get base type of array.
static Type* leafType(Type* ty) {
    if (ty->isArray()) return leafType(cast<ArrayType>(ty)->getElementType());
    return ty;
}

// Recursively collect int leaf values; ConstantZero expands to zeros.
static void collectInts(Constant* c, Type* ty, std::vector<int>& out) {
    if (!c || isa<ConstantZero>(c)) {
        int n = totalLeafCount(ty);
        for (int i = 0; i < n; i++) out.push_back(0);
        return;
    }
    if (isa<ConstantInt>(c)) {
        out.push_back(cast<ConstantInt>(c)->getValue());
        return;
    }
    if (isa<ConstantArray>(c)) {
        auto* ca = cast<ConstantArray>(c);
        auto* at = cast<ArrayType>(ty);
        for (auto* e : ca->getConsts())
            collectInts(e, at->getElementType(), out);
        return;
    }
    assert(false && "unexpected Constant kind in collectInts");
}

// Recursively collect float leaf values; ConstantZero expands to zeros.
static void collectFloats(Constant* c, Type* ty, std::vector<float>& out) {
    if (!c || isa<ConstantZero>(c)) {
        int n = totalLeafCount(ty);
        for (int i = 0; i < n; i++) out.push_back(0.0f);
        return;
    }
    if (isa<ConstantFloat>(c)) {
        out.push_back(cast<ConstantFloat>(c)->getValue());
        return;
    }
    if (isa<ConstantArray>(c)) {
        auto* ca = cast<ConstantArray>(c);
        auto* at = cast<ArrayType>(ty);
        for (auto* e : ca->getConsts())
            collectFloats(e, at->getElementType(), out);
        return;
    }
    assert(false && "unexpected Constant kind in collectFloats");
}

void AsmPrinter::run(const std::vector<std::unique_ptr<MCFunction>>& funcs,
                     Module* module, std::ostream& os) {
    emitText(funcs, os);
    emitGlobals(module, os);
}

void AsmPrinter::emitText(const std::vector<std::unique_ptr<MCFunction>>& funcs,
                           std::ostream& os) {
    os << ".text\n";
    for (size_t i = 0; i < funcs.size(); ++i) {
        auto& mcFunc = funcs[i];
        if (i > 0) os << "\n\n";
        os << ".align 2\n"; // align to 4 bytes
        if (mcFunc->name == "main")
            os << ".globl main\n";
        os << mcFunc->name << ":\n";
        for (auto& mcBB : mcFunc->blocks) {
            os << mcBB->name << ":\n";
            mcBB->forEach([&](RvOp* op) {
                op->print(os);
            });
        }
    }
}

void AsmPrinter::emitGlobals(Module* module, std::ostream& os) {
    auto& globals = module->getGlobals();
    if (globals.empty()) return;

    std::vector<GlobalVariable*> dataSeg, bssSeg;
    for (auto* gv : globals) {
        if (isAllZero(gv->getInit())) bssSeg.push_back(gv);
        else dataSeg.push_back(gv);
    }

    if (!dataSeg.empty()) {
        os << "\n\n.data\n";
        for (auto* gv : dataSeg) {
            // GlobalVariable::getType() returns PointerType(elementType); unwrap it.
            Type* ty = gv->getType();
            if (ty->isPointer()) ty = cast<PointerType>(ty)->getPointeeType();
            Type* leaf = leafType(ty);
            os << ".align 2\n";
            os << gv->getName() << ":\n";

            if (leaf->isInt()) {
                std::vector<int> vals;
                collectInts(gv->getInit(), ty, vals);
                os << "    .word";
                for (int i = 0; i < (int)vals.size(); i++)
                    os << (i == 0 ? " " : ", ") << vals[i];
                os << "\n";
            } else if (leaf->isFloat()) {
                // IEEE 754
                std::vector<float> vals;
                collectFloats(gv->getInit(), ty, vals);
                os << "    .word";
                for (int i = 0; i < (int)vals.size(); i++) {
                    uint32_t bits;
                    std::memcpy(&bits, &vals[i], 4);
                    os << (i == 0 ? " " : ", ") << bits;
                }
                os << "\n";
            }
        }
    }

    if (!bssSeg.empty()) {
        os << "\n\n.bss\n";
        for (auto* gv : bssSeg) {
            Type* bssTy = gv->getType();
            if (bssTy->isPointer()) bssTy = cast<PointerType>(bssTy)->getPointeeType();
            int bytes = totalLeafCount(bssTy) * 4;
            os << ".align 2\n";
            os << gv->getName() << ":\n";
            os << "    .space " << bytes << "\n";
        }
    }
}

} // namespace rv
} // namespace sysy
