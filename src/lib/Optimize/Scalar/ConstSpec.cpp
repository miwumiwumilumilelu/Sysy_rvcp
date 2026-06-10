#include "../../../include/Optimize/Scalar/ConstSpec.h"
#include "../../../include/Optimize/Scalar/IRClone.h"
#include "../../../include/Optimize/Analysis/PureFunc.h"
#include "../../../include/IR/Instruction.h"
#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

using namespace sysy;

Function* ConstSpec::cloneWithConsts(Function* orig, const ArgPattern& pat) {
    // Build a name that encodes the specialization pattern.
    std::string specName = orig->getName();
    auto encodeConst = [](int v) -> std::string {
        if (v >= 0) return "v" + std::to_string(v);
        return "vn" + std::to_string(-v);
    };
    for (auto [idx, val] : pat)
        specName += ".cs" + std::to_string(idx) + encodeConst(val);

    auto* spec = new Function(specName, orig->getType());

    // Build vmap: orig arg → ConstantInt (if specialized) or new Argument.
    ValueMap vmap;
    std::map<int, int> constMap(pat.begin(), pat.end());
    int newArgNo = 0;
    for (int i = 0; i < (int)orig->getArgs().size(); i++) {
        auto* origArg = orig->getArgs()[i];
        auto it = constMap.find(i);
        if (it != constMap.end()) {
            vmap[origArg] = new ConstantInt(it->second);
        } else {
            auto* newArg = new Argument(origArg->getType(),
                                        origArg->getName(), spec, newArgNo++);
            spec->addArgument(newArg);
            vmap[origArg] = newArg;
        }
    }

    // Pre-create all basic blocks (without auto-inserting into body yet).
    BlockMap bbMap;
    std::vector<BasicBlock*> origBlocks;
    for (auto* bb : orig->getBody()->getBlocks()) {
        origBlocks.push_back(bb);
        auto* cloned = new BasicBlock(bb->getName(), nullptr);
        cloned->setParent(spec->getBody());
        spec->getBody()->addBlock(cloned);
        bbMap[bb] = cloned;
    }

    // Two-pass clone: skeletons first so forward refs across blocks (e.g.
    // values hoisted by GVNHoist) resolve correctly when filling operands.
    for (auto* origBB : origBlocks)
        for (auto* inst : origBB->getInstructions())
            vmap[inst] = cloneSkeleton(inst, bbMap[origBB]);
    for (auto* origBB : origBlocks)
        for (auto* inst : origBB->getInstructions())
            fillOperands(cast<Instruction>(vmap[inst]), inst, vmap, bbMap);

    return spec;
}

static bool funcIsRecursive(Function* f) {
    const std::string& name = f->getName();
    for (auto* bb : f->getBody()->getBlocks())
        for (auto* inst : bb->getInstructions())
            if (auto* call = dyn_cast<CallInst>(inst))
                if (call->getFunction() == f)
                    return true;
    return false;
}

static bool recursiveArgChangesOnSelfCalls(Function* f, int argIdx) {
    auto* formal = f->getArgs()[argIdx];
    bool sawSelfCall = false;

    for (auto* bb : f->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            auto* call = dyn_cast<CallInst>(inst);
            if (!call || call->getFunction() != f) continue;
            sawSelfCall = true;

            // getOperand(0)=callee, getOperand(i+1)=arg i.
            if (argIdx + 1 >= call->getNumOperands()) return false;
            if (call->getOperand(argIdx + 1) == formal)
                return false;
        }
    }
    return sawSelfCall;
}

static bool hasSideEffectingNonSelfCall(Function* f) {
    std::unordered_map<Function*, bool> purityCache;
    for (auto* bb : f->getBody()->getBlocks()) {
        for (auto* inst : bb->getInstructions()) {
            auto* call = dyn_cast<CallInst>(inst);
            if (!call) continue;
            Function* callee = call->getFunction();
            if (!callee || callee == f) continue;
            if (!isPureFunc(callee, purityCache))
                return true;
        }
    }
    return false;
}

ConstSpec::ArgPattern ConstSpec::getPattern(CallInst* call) {
    ArgPattern pat;
    Function* callee = call->getFunction();

    bool limitToFirst = funcIsRecursive(callee);
    if (limitToFirst &&
        !callee->getType()->isVoid() &&
        hasSideEffectingNonSelfCall(callee))
        return pat;

    for (int i = 1; i < call->getNumOperands(); i++) {
        if (auto* ci = dyn_cast<ConstantInt>(call->getOperand(i))) {
            if (limitToFirst && !recursiveArgChangesOnSelfCalls(callee, i - 1))
                continue;
            pat.push_back({i - 1, ci->getValue()});
            if (limitToFirst) break;
        }
    }
    return pat;
}

Function* ConstSpec::getOrCreate(Function* callee, const ArgPattern& pat) {
    auto key = std::make_pair(callee, pat);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    if (cloneCount >= MaxClones) return nullptr;

    auto* spec = cloneWithConsts(callee, pat);
    cache[key] = spec;
    isSpec.insert(spec);
    TheModule->addFunction(spec);
    ++cloneCount;
    return spec;
}

static void redirectCall(CallInst* call, Function* spec,
                         const std::vector<std::pair<int,int>>& pat) {
    // Build the set of arg indices that were specialized away.
    std::set<int> specIdxs;
    for (auto [idx, val] : pat) specIdxs.insert(idx);

    // Collect args for the new call, skipping specialized ones.
    // call->getOperand(0) = callee; getOperand(i+1) = arg i.
    std::vector<Value*> newArgs;
    for (int i = 1; i < call->getNumOperands(); i++) {
        int argIdx = i - 1;
        if (!specIdxs.count(argIdx))
            newArgs.push_back(call->getOperand(i));
    }

    BasicBlock* bb = call->getParent();
    auto* newCall = new CallInst(spec, newArgs, nullptr);
    newCall->setName(call->getName());
    newCall->setParent(bb);

    // Insert new call at the same position as the old one.
    auto& insts = bb->getInstructions();
    auto it = std::find(insts.begin(), insts.end(), call);
    insts.insert(it, newCall);

    // Replace all uses of the old call result with the new one.
    call->replaceAllUsesWith(newCall);

    // Erase the old call (it == position of old call after insert).
    (void)it;
    call->eraseInst();
}

bool ConstSpec::run() {
    bool anyChanged = false;

    std::vector<Function*> funcs(TheModule->getFunctions().begin(),
                                 TheModule->getFunctions().end());

    for (auto* func : funcs) {
        std::vector<std::pair<CallInst*, ArgPattern>> candidates;
        for (auto* bb : func->getBody()->getBlocks()) {
            for (auto* inst : bb->getInstructions()) {
                auto* call = dyn_cast<CallInst>(inst);
                if (!call) continue;
                Function* callee = call->getFunction();
                // Skip: external (no body), or already a specialization.
                if (!callee || callee->getBody()->getBlocks().empty()) continue;
                if (isSpec.count(callee)) continue;
                ArgPattern pat = getPattern(call);
                if (pat.empty()) continue;
                candidates.push_back({call, std::move(pat)});
            }
        }

        std::set<uintptr_t> processed;

        for (auto& [call, pat] : candidates) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(call);

            if (processed.count(addr)) continue;

            if (call->getParent() == nullptr) continue;

            Function* callee = call->getFunction();
            if (!callee || isSpec.count(callee)) continue;

            auto* spec = getOrCreate(callee, pat);
            if (!spec) continue;

            redirectCall(call, spec, pat);
            processed.insert(addr);
            anyChanged = true;
        }
    }

    return anyChanged;
}
