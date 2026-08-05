#ifndef RECURSIVEMODMUL_H
#define RECURSIVEMODMUL_H
#include "../../IR/Module.h"
namespace sysy { class RecursiveModMul { Module* M; public: explicit RecursiveModMul(Module* m):M(m){} bool run(); }; }
#endif
