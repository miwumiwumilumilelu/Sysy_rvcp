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
        FunctionTy,
        ArrayTy,
        TensorTy
    };

    explicit Type(TypeID id) : ID(id) {}
    virtual ~Type() = default;

    TypeID getTypeID() const { return ID; }

    bool isInt() const { return ID == IntTy; }
    bool isFloat() const { return ID == FloatTy; }
    bool isVoid() const { return ID == VoidTy; }
    bool isPointer() const { return ID == PointerTy; }
    bool isArray() const { return ID == ArrayTy; }
    bool isTensor() const { return ID == TensorTy; }

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

    static bool classof(const Type* T) {
        return T->getTypeID() == IntTy;
    }
};

class FloatType : public Type {
public:
    FloatType() : Type(FloatTy) {}

    std::string toString() const override { return "float"; }

    static bool classof(const Type* T) {
        return T->getTypeID() == FloatTy;
    }
};

class VoidType : public Type {
public:
    VoidType() : Type(VoidTy) {}

    std::string toString() const override { return "void"; }

    static bool classof(const Type* T) {
        return T->getTypeID() == VoidTy;
    }
};

class PointerType : public Type {
    Type* PointeeTy;
public:
    PointerType(Type* pointee) : Type(PointerTy), PointeeTy(pointee) {}

    Type* getPointeeType() const { return PointeeTy; }
    std::string toString() const override { return PointeeTy->toString() + "*"; }

    static bool classof(const Type* T) {
        return T->getTypeID() == PointerTy;
    }
};

class LabelType : public Type {
public:
    LabelType() : Type(LabelTy) {}

    std::string toString() const override { return "label"; }

    static bool classof(const Type* T) {
        return T->getTypeID() == LabelTy;
    }
};

class ArrayType : public Type {
    Type* ElemTy;
    int NumElements;
public:
    ArrayType(Type* elem, int num) : Type(ArrayTy), ElemTy(elem), NumElements(num) {}

    Type* getElementType() const { return ElemTy; }
    int getNumElements() const { return NumElements; }

    std::string toString() const override {
        return "[" + std::to_string(NumElements) + " x " + ElemTy->toString() + "]";
    }

    static bool classof(const Type* T) {
        return T->getTypeID() == ArrayTy;
    }
};

class TensorType : public Type {
    Type* ElemTy;
public:
    TensorType(Type* elemty) : Type(TensorTy), ElemTy(elemty) {}

    Type* getElemTy() const { return ElemTy; }

    std::string toString() const override {
        return "tensor<" + ElemTy->toString() + ">";
    }

    static bool classof(const Type* T) {
        return T->getTypeID() == TensorTy;
    }
};

}

#endif 