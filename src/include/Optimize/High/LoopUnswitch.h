#ifndef LOOPUNSWITCH_H
#define LOOPUNSWITCH_H

#include "IR/Module.h"
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sysy {

class LoopUnswitch {
private:
    struct InvariantInfo {
        IfInst* guard = nullptr;
        std::vector<Instruction*> prefix;
    };

    struct ForMatchInfo {
        IfInst* guard = nullptr;
        Value* splitBase = nullptr;
        int splitOffset = 0;
        int step = 1;
        bool increasing = true;
        bool firstTakesThen = false;
        std::vector<Instruction*> prefix;
        std::vector<Instruction*> suffix;
    };

    struct LoopSplitPlan {
        ForInst* source = nullptr;
        ForMatchInfo match;
    };

    struct ModMatchInfo {
        int A = 0;
        int startVal = 0;
        int stepVal = 1;
    };

    struct IVExpr {
        bool matched = false;
        int offset = 0;
    };

    struct ModExpr {
        bool matched = false;
        IVExpr dividend;
        int divisor = 0;
    };

    using InstIter = std::list<Instruction*>::iterator;

    Module* M;
    std::set<ForInst*> sideLoops_;

    // IR analysis
    void collectLoadAddrs(Value* v, std::set<Value*>& addrs, std::set<Value*>& visiting);
    Value* stripGEP(Value* v);
    bool regionStoresToBase(Region* region, Value* base);
    bool instStoresToBase(Instruction* inst, Value* base);
    bool loadsStableInLoop(Region* region, Value* ivAddr,
                           const std::set<Value*>& loadAddrs,
                           const std::set<Value*>& loopDefs);
    int instCost(Instruction* inst);
    int regionCost(Region* region);
    bool splitCostIsSmall(ForInst* fi, IfInst* guard,
                          const std::vector<Instruction*>& prefix,
                          const std::vector<Instruction*>& suffix);
    bool selectedRegionIsSimple(IfInst* guard, bool takeThen);
    void collectDefs(Region* region, std::set<Value*>& defs);
    bool valueDependsOnLoop(Value* v, const std::set<Value*>& loopDefs, std::set<Value*>& visiting);
    bool isInvariantValue(Value* v, const std::set<Value*>& loopDefs);
    bool absDivisor(int value, int& out);
    bool isLoadFrom(Value* v, Value* addr);
    bool getConstInt(Value* v, int& value);
    IVExpr matchIVExpr(Value* v, Value* ivAddr);
    ModExpr matchIVModExpr(Value* v, Value* ivAddr);

    // Loop transformation
    Value* addConst(Value* v, int delta, BasicBlock* bb, InstIter pos,
                    const std::string& name = "loop.add");
    Value* matBefore(Value* v, BasicBlock* src, BasicBlock* dst, InstIter pos,
                     std::map<Value*, Value*>& vmap);
    Value* alignStart(Value* start, Value* split, int step, bool inc,
                      BasicBlock* bb, InstIter pos,
                      const std::string& tag = "loop.align");

    // Invariant branch unswitching
    bool runInvariantFunc(Function* f);
    bool processInvariantRegion(Region* region);
    bool processInvariantWhile(WhileInst* wi);
    bool processInvariantFor(ForInst* fi);
    bool applyInvariant(
        Instruction* loop,
        const InvariantInfo& inv,
        const std::string& tag,
        const std::vector<ResultValue*>& sourceResults,
        const std::function<Instruction*(std::map<Value*, Value*>&)>& cloneLoop,
        const std::function<void(Instruction*, std::vector<Value*>&)>& collectFlow,
        const std::function<void(const std::vector<ResultValue*>&)>& replaceResults);

    bool guardLoadsStable(WhileInst* wi, IfInst* guard,
                          const std::vector<Instruction*>& prefix);
    bool findInvariant(WhileInst* wi, InvariantInfo& ii);
    bool findInvariantFor(ForInst* fi, InvariantInfo& ii);
    IfInst* firstIfWithPrefix(BasicBlock* bodyBB, std::vector<Instruction*>& prefix);
    bool prefixIsPure(const std::vector<Instruction*>& prefix);
    bool valueUsesAny(Value* v, const std::set<Value*>& defs, std::set<Value*>& visiting);
    bool buildInvariantPrefix(Value* guardCond,
                              const std::vector<Instruction*>& bodyPrefix,
                              std::vector<Instruction*>& outerPrefix);

    // IV loop splitting
    bool runIV();
    bool runIVFunc(Function* f);
    bool processIVRegion(Region* region);
    bool processIVFor(ForInst* fi);

    ICmpInst::CmpOp swapPred(ICmpInst::CmpOp pred);
    bool loadsAreStableInFor(ForInst* fi, Value* ivAddr, IfInst* guard,
                             const std::vector<Instruction*>& prefix);
    bool guardTrueIsEarly(ICmpInst::CmpOp pred, bool increasing, bool& early);
    bool regionIsPureContinue(Region* region);
    bool instHasWork(Instruction* inst);
    bool regionHasWork(Region* region);
    bool suffixHasWork(const std::vector<Instruction*>& suffix);
    bool pathHasWork(IfInst* guard, bool takeThen, const std::vector<Instruction*>& suffix);
    bool hasOpaqueCall(Instruction* inst);
    bool hasOpaqueCall(Region* region);
    bool pathFallsThrough(IfInst* guard, bool takeThen);
    bool matchIneq(Value* cond, Value* ivAddr, ICmpInst::CmpOp& pred,
                   IVExpr& ivExpr, Value*& bound);
    bool findForMatch(ForInst* fi, ForMatchInfo& mi);
    bool fillSplitBody(ForInst* loop, IfInst* guard, bool takeThen,
                       const ForMatchInfo& mi);
    bool applyLoopSplit(const LoopSplitPlan& plan);

    // --- Mod unrolling ---
    bool runMod();
    bool runModFunc(Function* f);
    bool processModRegion(Region* region);
    bool processModFor(ForInst* fi);

    bool isFlatCloneableForUnroll(Instruction* inst);
    bool matchMod(ForInst* fi, ModMatchInfo& mi);
    void tidyMod(ForInst* fi, int vi, int startVal, int stepVal);
    void tidyModRegion(Region* region, Value* ivAddr, int period, int startVal);
    void subIVLoads(Region* region, Value* ivAddr, Value* ivZ);
    void subIVLoadsInClone(Instruction* inst, Value* ivAddr, Value* ivZ);
    void cloneBodyInto(BasicBlock* targetBB,
                       const std::vector<Instruction*>& body,
                       std::map<Value*, Value*>& vmap,
                       const std::string& suffix,
                       Value* ivAddr = nullptr,
                       Value* ivValue = nullptr);
    void cloneUnrolledBody(ForInst* fi,
                           const std::vector<Instruction*>& body,
                           Value* ivBase, int vi, int stepVal);
    Value* buildAlignedEnd(ForInst* fi, int vi, int startVal, int stepVal,
                           BasicBlock* parentBB, InstIter pos);
    ForInst* buildSideLoop(ForInst* fi,
                           const std::vector<Instruction*>& body,
                           Value* alignedEnd, Value* origStop, int stepVal,
                           BasicBlock* parentBB, InstIter pos);
    ForInst* unrollFor(ForInst* fi, int vi, int startVal, int stepVal,
                       BasicBlock* parentBB, InstIter pos);

public:
    explicit LoopUnswitch(Module* m) : M(m) {}
    bool runInvariant();
    bool run();
};

}

#endif
