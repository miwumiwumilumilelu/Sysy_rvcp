#ifndef VALUE_H
#define VALUE_H

#include "IR/Type.h"
#include <string>
#include <vector>
#include <iostream>

namespace sysy {
    
class Value {
protected:
    Type* Ty;
    std::string Name;

public:
    Value(Type* ty, const std::string &name = "") : Ty(ty), Name(name) {}
    virtual ~Value() = default;

    Type* getType() const { return Ty; }
    std::string getName() const { return Name; }
    void setName(const std::string &name) { Name = name; }

    virtual std::string toString() const = 0;
};

class User : public Value {
protected:
    std::vector<Value*> Operands;
public:
    User(Type* ty, const std::string &name = "") : Value(ty, name) {}

    void addOperand(Value* v) { Operands.push_back(v); }
    Value* getOperand(int i) const { return Operands[i]; }
    int getNumOperands() const { return Operands.size(); }
};

class Constant : public User {
public:
    Constant(Type* ty, const std::string &name = "") : User(ty, name) {}
};

class ConstantInt : public Constant {
    int Val;
public:
    ConstantInt(int val) : Constant(Type::getIntTy(), std::to_string(val)), Val(val) {}
    int getValue() const { return Val; }
    std::string toString() const override { return std::to_string(Val); }
};

class ConstantZero : public Constant {
public:
    ConstantZero(Type* ty) : Constant(ty, "") {}
    std::string toString() const override { return "zeroinitializer"; }
};

class ConstantArray : public Constant {
    std::vector<Constant*> Consts;
public:
    ConstantArray(ArrayType* ty, const std::vector<Constant*> &consts) 
        : Constant(ty, ""), Consts(consts) {}
    
    std::string toString() const override;
};

class GlobalVariable : public User {
public:
    GlobalVariable(const std::string &name, Type* ty, Constant* initVal = nullptr);

    bool isConst() const { return IsConst; }
    void setConst(bool c) { IsConst = c; }

    Constant* getInit() const { return InitVal; }

    std::string toString() const override;
private:
    bool IsConst = false;
    Constant* InitVal;
};

}
#endif