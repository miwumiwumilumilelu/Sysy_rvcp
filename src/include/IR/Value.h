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

class ConstantInt : public User {
    int Val;
public:
    ConstantInt(int val) : User(Type::getIntTy(), std::to_string(val)), Val(val) {}
    int getValue() const { return Val; }
    std::string toString() const override { return std::to_string(Val); }
};

}
#endif