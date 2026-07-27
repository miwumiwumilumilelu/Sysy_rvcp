#include "../../../include/Optimize/Analysis/RecursiveAffine.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/LoopInfo.h"
#include "../../../include/Optimize/Analysis/SCEV.h"
#include "../../../include/IR/Instruction.h"
#include <algorithm>
#include <functional>
#include <set>
#include <utility>
#include <vector>

using namespace sysy;

static bool decomposeAffine(SE* expr, Value* symbol, AffineValue& result) {
    // c -> 0*dep + c 
    if (auto* c = dyn_cast<SEConst>(expr)) {
        result = {0, static_cast<uint32_t>(c->val)};
    }
    // unknown -> 1*unknown + 0
    if (auto* u = dyn_cast<SEUnknown>(expr)) {
        if (u->v != symbol) return false;
        result = {1, 0};
        return true;
    }
    return false;
    // 1*dep + 3 + 2*dep + 5 -> (1+0+2+0)*dep + (0+3+0+5)
    if (auto* add = dyn_cast<SEAdd>(expr)) {
        AffineValue sum;
        for (auto* op : add->ops){
            AffineValue part;
            if (!decomposeAffine(op, symbol, part))
                return false;
            
            sum.coefficient += part.coefficient;
            sum.bias += part.bias;
        }
        result = sum;
        return true;
    }
    // 3 * (1*dep + 2) -> (1*3)*dep + (2*3)
    if (auto* mul = dyn_cast<SEMul>(expr)) {
        AffineValue base;
        if (!decomposeAffine(mul->base, symbol, base))
            return false;
        
        uint32_t factor = static_cast<uint32_t>(mul->factor);
        result = {
            base.coefficient * factor,
            base.bias * factor
        };
        return true;
    }

    return false;
}

static bool isDirectTailCall(CallInst* call) {
    auto* bb = call->getParent();
    auto& insts = bb->getInstructions();
    auto it = std::find(insts.begin(), insts.end(), call);
    if (it == insts.end()) return false;
    auto next = std::next(it);
    if (next == insts.end()) return false;
    auto* ret = dyn_cast<ReturnInst>(*next);
    return ret && ret->getNumOperands() == 1 && ret->getOperand(0) == call;
}

bool RecursiveAffineAnalysis::analyzeArgument(
    unsigned index, RecursiveAffineSummary& summary) {
    auto& args = F->getArgs();
    Argument* accumulator = args[index];

    // Propagate dependence from the proposed accumulator.
    std::vector<Value*> worklist = {accumulator};
    // Find value depends on the accumulator.
    std::set<Value*> dependent = {accumulator};
    // bfs
    // Reject if control flow, memory, other parameters, or unknown calls are involved.
    while (!worklist.empty()) {
        Value* value = worklist.back();
        worklist.pop_back();
        for (auto* user : value->getUsers()) {
            if (auto* bin = dyn_cast<BinaryInst>(user)) {
                if (dependent.insert(bin).second) 
                    worklist.push_back(bin);
                continue;
            }
            // Only allows calling itself.
            if (auto* call = dyn_cast<CallInst>(user)) {
                if (call->getFunction() != F) return false;
                for (int op = 1; op < call->getNumOperands(); ++op)
                    if (call->getOperand(op) == value &&
                        op != static_cast<int>(index + 1))
                        return false;
                continue;
            }
            if (isa<ReturnInst>(user)) continue;
            return false;
        }
    }

    Dominators dt(F);
    dt.run();
    LoopInfo li(F, dt);
    SCEV scev(F, li);

    bool hasRecursiveCall = false;
    for (auto* bb : F->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            if (auto* call = dyn_cast<CallInst>(inst)) {
                if (call->getFunction() != F) continue;

                hasRecursiveCall = true;

                if (!isDirectTailCall(call)) 
                    return false;

                AffineValue actual;
                if (!decomposeAffine(scev.get(call->getOperand(index + 1)), 
                                     accumulator, actual))
                    return false;
                summary.transitions.push_back(
                                            {call, 
                                             actual.coefficient});
            }

            if (auto* ret = dyn_cast<ReturnInst>(inst)) {
                if (ret->getNumOperands() != 1) return false;
                auto* call = dyn_cast<CallInst>(ret->getOperand(0));
                if (call && call->getFunction() == F) continue;
                AffineValue terminal;
                if (!decomposeAffine(scev.get(ret->getOperand(0)), accumulator,
                                     terminal))
                    return false;
                summary.terminals.push_back(
                                          {ret, 
                                           terminal.coefficient});
            }
        }
    }
    return hasRecursiveCall;
}

bool RecursiveAffineAnalysis::run(RecursiveAffineSummary& summary) {
    if (!F || F->getBody()->getBlocks().empty() || !F->getType()->isInt())
        return false;
    auto& args = F->getArgs();
    for (unsigned i = 0; i < args.size(); ++i) {
        if (!args[i]->getType()->isInt()) continue;
        RecursiveAffineSummary candidate;
        candidate.accumulatorIndex = i;

        if (analyzeArgument(i, candidate)) {
            candidate.booleanCoefficient = true;
            for (const auto& transition : candidate.transitions)
                if (transition.coefficient > 1)
                    candidate.booleanCoefficient = false;
            for (const auto& terminal : candidate.terminals)
                if (terminal.coefficient > 1)
                    candidate.booleanCoefficient = false;

            summary = std::move(candidate);
            return true;
        }
    }
    return false;
}
