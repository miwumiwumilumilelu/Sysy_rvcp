#ifndef TYPE_H
#define TYPE_H

#include <string>

namespace sysy {

class Type {
public:
    enum TypeID {
        VoidTy,
        IntTy,
        FloatTy,
        PointerTy,
        LabelTy,
        FunctionTy
    };

    explicit Type(TypeID id) : ID(id) {}
    virtual ~Type() = default;

    bool isInt() const { return ID == IntTy; }
    bool isVoid() const { return ID == VoidTy; }
    bool isPointer() const { return ID == PointerTy; }

    virtual std::string toString() const = 0;

    static Type* getIntTy();
    static Type* getVoidTy();
    static Type* getFloatTy();
    static Type* getLabelTy();
private:
    TypeID ID;
};

class IntegerType : public Type {
public:
    IntegerType() : Type(IntTy) {}

    std::string toString() const override { return "i32"; }
};

class FloatType : public Type {
public:
    FloatType() : Type(FloatTy) {}

    std::string toString() const override { return "float"; }
};

class VoidType : public Type {
public:
    VoidType() : Type(VoidTy) {}

    std::string toString() const override { return "void"; }
};

class PointerType : public Type {
    Type* PointeeTy;
public:
    PointerType(Type* pointee) : Type(PointerTy), PointeeTy(pointee) {}

    std::string toString() const override { return PointeeTy->toString() + "*"; }
};

class LabelType : public Type {
public:
    LabelType() : Type(LabelTy) {}

    std::string toString() const override { return "label"; }
};

}

#endif 