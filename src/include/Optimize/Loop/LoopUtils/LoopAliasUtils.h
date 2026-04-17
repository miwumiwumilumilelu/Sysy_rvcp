#ifndef LOOPALIASUTILS_H
#define LOOPALIASUTILS_H

#include "IR/Value.h"
#include <set>

namespace sysy {

Value* getLoopBaseObject(Value* v, std::set<Value*>& vis);
Value* getLoopBaseObject(Value* v);

void collectAllBases(Value* v, std::set<Value*>& vis, std::set<Value*>& bases);

}
#endif
