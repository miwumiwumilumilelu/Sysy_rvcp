#ifndef ALIASANALYSIS_H
#define ALIASANALYSIS_H

#include "../../IR/Value.h"

namespace sysy {

class SCEV;

class AliasAnalysis {
public:
    enum class Result { NoAlias, MayAlias, MustAlias };
    Result query(Value* a, Value* b, SCEV* scev = nullptr) const;
    bool mayAlias(Value* a, Value* b, SCEV* scev = nullptr) const {
        return query(a, b, scev) != Result::NoAlias;
    }

private:
    static Value* getBase(Value* ptr);
};

}
#endif
