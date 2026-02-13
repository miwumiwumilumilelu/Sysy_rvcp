#ifndef VALUE_H
#define VALUE_H

#include "IR/Type.h"
#include <string>
#include <vector>
#include <iostream>
#include <cassert>

namespace sysy {

// RTTI templates.
template<typename To, typename From>
bool isa(const From* val);

template<typename To, typename From>
To* cast(From* val);

template<typename To, typename From>
To* dyn_cast(From* val);

class User;
    
class Value {
public:
    enum ValueKind {
        VK_Argument,
        VK_BasicBlock,
        VK_Function,
        VK_GlobalVariable,
        VK_ConstantInt,
        VK_ConstantFloat,
        VK_ConstantZero,
        VK_ConstantArray,
        VK_Instruction,
    };

protected:
    Type* Ty;
    std::string Name;
    const ValueKind VKind;
    std::vector<User*> UseList;

public:
    Value(Type* ty, ValueKind kind, const std::string &name = "") : Ty(ty), Name(name), VKind(kind) {}
    virtual ~Value() = default;

    Type* getType() const { return Ty; }
    std::string getName() const { return Name; }
    void setName(const std::string &name) { Name = name; }

    ValueKind getValueKind() const { return VKind; }

    virtual std::string toString() const = 0;

    void addUser(User* user) { UseList.push_back(user); }
    void removeUser(User* user) {
        auto it = std::remove(UseList.begin(), UseList.end(), user);
        UseList.erase(it, UseList.end());
    }   

    const std::vector<User*>& getUsers() const { return UseList; }

    void replaceAllUsesWith(Value* newVal);
};

class User : public Value {
protected:
    std::vector<Value*> Operands;
public:
    User(Type* ty, ValueKind kind, const std::string &name = "") : Value(ty, kind, name) {}

    virtual ~User() {
        for (auto v : Operands) {
            if (v) v->removeUser(this);
        }
    }

    void addOperand(Value* v) { 
        Operands.push_back(v); 
        if (v) v->addUser(this);
    }
    Value* getOperand(int i) const { return Operands[i]; }
    int getNumOperands() const { return Operands.size(); }
    void setOperand(int i, Value* v) { 
        if (i < Operands.size()) {
            Value* oldVal = Operands[i];
            if (oldVal) oldVal->removeUser(this);
            
            Operands[i] = v;
            if (v) v->addUser(this);
        }
    }
};

inline void Value::replaceAllUsesWith(Value* newVal) {
    std::vector<User*> users = UseList;
    
    for (auto user : users) {
        for (int i = 0; i < user->getNumOperands(); ++i) {
            if (user->getOperand(i) == this) {
                user->setOperand(i, newVal);
            }
        }
    }
}

class Constant : public User {
public:
    Constant(Type* ty, ValueKind kind, const std::string &name = "") : User(ty, kind, name) {}

    static bool classof(const Value* v) {
        return v->getValueKind() >= VK_ConstantInt && v->getValueKind() <= VK_ConstantArray;
    }
};

class ConstantInt : public Constant {
    int Val;
public:
    ConstantInt(int val) : Constant(Type::getIntTy(), VK_ConstantInt, std::to_string(val)), Val(val) {}
    int getValue() const { return Val; }
    std::string toString() const override { return std::to_string(Val); }

    static bool classof(const Value* v) {
        return v->getValueKind() == VK_ConstantInt;
    }
};

class ConstantFloat : public Constant {
    float Val;
public:
    ConstantFloat(float val) : Constant(Type::getFloatTy(), VK_ConstantFloat, std::to_string(val)), Val(val) {}
    float getValue() const { return Val; }
    std::string toString() const override { return std::to_string(Val); }

    static bool classof(const Value* v) {
        return v->getValueKind() == VK_ConstantFloat;
    }
};

class ConstantZero : public Constant {
public:
    ConstantZero(Type* ty) : Constant(ty, VK_ConstantZero, "") {}
    std::string toString() const override { return "zeroinitializer"; }

    static bool classof(const Value* v) {
        return v->getValueKind() == VK_ConstantZero;
    }
};

class ConstantArray : public Constant {
    std::vector<Constant*> Consts;
public:
    ConstantArray(ArrayType* ty, const std::vector<Constant*> &consts) 
        : Constant(ty, VK_ConstantArray, ""), Consts(consts) {}
    
    std::string toString() const override;

    static bool classof(const Value* v) {
        return v->getValueKind() == VK_ConstantArray;
    }
};

class GlobalVariable : public User {
public:
    GlobalVariable(const std::string &name, Type* ty, Constant* initVal = nullptr);

    bool isConst() const { return IsConst; }
    void setConst(bool c) { IsConst = c; }

    Constant* getInit() const { return InitVal; }

    std::string toString() const override;

    static bool classof(const Value* v) {
        return v->getValueKind() == VK_GlobalVariable;
    }
private:
    bool IsConst = false;
    Constant* InitVal;
};

template<typename To, typename From>
bool isa(const From* val) {
    return val && To::classof(val);
}

template<typename To, typename From>
To* cast(From* val) {
    assert(isa<To>(val) && "cast<To>() argument of incompatible type!");
    return (To*)val;
}

template<typename To, typename From>
To* dyn_cast(From* val) {
    return isa<To>(val) ? (To*)val : nullptr;
}

}
#endif