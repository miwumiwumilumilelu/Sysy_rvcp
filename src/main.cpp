#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include "include/Lex/Lexer.h"
#include "include/Parse/Parser.h"
#include "include/Semant/Semant.h"
#include "include/IR/IRGen.h"
#include "include/Optimize/CFG/FlattenCFG.h"
#include "include/Optimize/High/HighMem2Reg.h"
#include "include/Optimize/High/HighLICM.h"
#include "include/Optimize/High/WhileToFor.h"
#include "include/Optimize/High/HighDCE.h"
#include "include/Optimize/High/LowerFor.h"
#include "include/Optimize/High/LoopUnswitch.h"
#include "include/Optimize/Analysis/Dominators.h"
#include "include/Optimize/Scalar/Mem2Reg.h"
#include "include/Optimize/Scalar/ConstantFold.h"
#include "include/Optimize/Scalar/CSE.h"
#include "include/Optimize/Scalar/GVN.h"
#include "include/Optimize/Scalar/GVNHoist.h"
#include "include/Optimize/Scalar/InstSimplify.h"
#include "include/Optimize/CFG/SimplifyCFG.h"
#include "include/Optimize/Scalar/DCE.h"
#include "include/Optimize/Scalar/DSE.h"
#include "include/Optimize/Scalar/DFE.h"
#include "include/Optimize/Scalar/StrengthReduce.h"
#include "include/Optimize/Scalar/TailCallElim.h"
#include "include/Optimize/Scalar/Memoize.h"
#include "include/Optimize/Scalar/SSAInline.h"
#include "include/Optimize/Scalar/ConstSpec.h"
#include "include/Optimize/Scalar/LoopUnroll.h"
#include "include/Optimize/Loop/LoopSimplify.h"
#include "include/Optimize/Loop/LoopRotate.h"
#include "include/Optimize/Loop/LCSSA.h"
#include "include/Optimize/Loop/DeadLoopElim.h"
#include "include/Optimize/Loop/LICM.h"
#include "include/Optimize/Loop/LoopExitFold.h"
#include "include/Optimize/Loop/LoopMemPromote.h"
#include "include/Optimize/Loop/SubloopHoist.h"
#include "include/Optimize/Loop/LoopStrengthReduce.h"
#include "include/Optimize/Loop/LoopGVN.h"
#include "include/rv/InstSel.h"
#include "include/rv/RegAlloc.h"
#include "include/rv/MCPeephole.h"
#include "include/rv/MCInvariantHoist.h"
#include "include/rv/AsmPrinter.h"

using namespace sysy;
using namespace sysy::rv;

static bool runCleanup(Module* m, std::function<bool(const std::string&)> dump = nullptr) {
    bool anyChanged = false;
    bool c = true;
    int iter = 0;
    while (c) {
        c = false;
        ++iter;
        auto d = [&](const std::string& stage) -> bool {
            if (!dump) return false;
            return dump("sc" + std::to_string(iter) + "-" + stage);
        };
        c |= ConstantFold(m).run(); if (d("cf")) return anyChanged;
        c |= CSE(m).run();          if (d("cse")) return anyChanged;
        c |= GVN(m).run();          if (d("gvn")) return anyChanged;
        c |= GVNHoist(m).run();     if (d("gvnh")) return anyChanged;
        c |= InstSimplify(m).run();       if (d("is")) return anyChanged;
        c |= StrengthReduce(m).run();     if (d("sr")) return anyChanged;
        c |= SimplifyCFG(m).run();        if (d("scfg")) return anyChanged;
        c |= DSE(m).run();          if (d("dse")) return anyChanged;
        c |= DCE(m).run();          if (d("dce")) return anyChanged;
        anyChanged |= c;
    }
    return anyChanged;
}

int main(int argc, char **argv) {
    std::string inputFile;
    std::string outputFile;
    bool dumpHIR = false;
    bool dumpLIR = false;
    std::string dumpAfterPass;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o") {
            if (i + 1 < argc) {
                outputFile = argv[++i];
            } else {
                std::cerr << "Error: -o expects a filename\n";
                return 1;
            }
        } else if (arg.rfind("-o", 0) == 0 && arg.size() > 2) {
            outputFile = arg.substr(2);
        } else if (arg == "-S") {
            // Assembly output is the default backend mode.
        } else if (arg.rfind("-O", 0) == 0) {
            // no-op
        } else if (arg == "--dump-mid-ir") {
            dumpHIR = true;
        } else if (arg == "--dump-cfg-ir") {
            dumpLIR = true;
        } else if (arg.rfind("--dump-", 0) == 0 && arg.size() > 10 &&
                   arg.substr(arg.size() - 3) == "-ir") {
            dumpAfterPass = arg.substr(7, arg.size() - 10);
        } else if (!arg.empty() && arg[0] == '-') {
            // Ignore driver/compiler flags that are irrelevant to this compiler.
            continue;
        } else {
            inputFile = arg;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: No input file specified.\n";
        return 1;
    }

    std::ifstream file(inputFile);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << inputFile << "\n";
        return 1;
    }
    std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

// ======== Frontend ========

    Lexer lexer(code);
    Parser parser(lexer);
    auto ast = parser.parseCompUnit();

    if (!ast) {
        return 1;
    }

    Semant semant;
    semant.visit(*ast);

    IRGen irGen;
    irGen.visit(*ast);

    auto module = irGen.getModule();

    auto ok = [&](const std::string& passName) -> bool {
        if (dumpAfterPass == passName) {
            std::cout << module->print();
            return true;
        }
        return false;
    };
    auto okIter = [&](const std::string& passPrefix, int iter, const std::string& stage) -> bool {
        return ok(passPrefix + std::to_string(iter) + "-" + stage);
    };

    if (dumpHIR) {
        std::cout << module->print();
        return 0;
    }

    if (ok("frontend")) return 0;

// ======== Structured High IR ========

    LoopUnswitch(module.get()).runInvariant();
    if (ok("liunswitch")) return 0;

    WhileToFor(module.get()).run();
    if (ok("whiletofor")) return 0;

    LoopUnswitch(module.get()).run();
    if (ok("unswitch")) return 0;

    HighDCE(module.get()).run();
    if (ok("hdce")) return 0;

    LowerFor(module.get()).run();
    if (ok("lowerfor")) return 0;

    HighMem2Reg highMem2Reg(module.get());
    highMem2Reg.run();
    if (ok("hmem2reg")) return 0;

    HighLICM highLICM(module.get());
    highLICM.run();
    if (ok("hlicm")) return 0;

// ======== Flattened CFG ========

    FlattenCFG flatten(module.get());
    flatten.run();
    if (ok("flatten")) return 0;

// ======== Mem2Reg ========

    Mem2Reg mem2reg(module.get(), nullptr);
    mem2reg.run();
    if (ok("mem2reg")) return 0;

// ======== Scalar Cleanup ========

    runCleanup(module.get(), ok);

// ======== Memoization ========

    Memoize(module.get()).run();
    if (ok("memoize")) return 0;

// ======== Tail Call Elimination ========

    TCE(module.get()).run();
    if (ok("tce")) return 0;

// ======== Inline ========

    while (SSAInline(module.get()).run()) {
        runCleanup(module.get());
    }
    if (ok("inline")) return 0;

// ======== ConstSpec ========

    // {
    //     ConstSpec constSpec(module.get());
    //     while (constSpec.run()) {
    //         runCleanup(module.get());
    //     }
    // }
    // if (ok("constspec")) return 0;

// ======== Post-ConstSpec Inline ========

    // while (SSAInline(module.get()).run()) {
    //     runCleanup(module.get());
    // }
    // if (ok("postinline")) return 0;

// ======== Dead Function Elimination ========

    DFE(module.get()).run();
    if (ok("dfe")) return 0;

// ======== StrengthReduce ========

    if (StrengthReduce(module.get()).run()) {
        runCleanup(module.get());
    }
    if (ok("sr")) return 0;

// ======== Loop Canonicalization ========

    LoopSimplify(module.get()).run();
    if (ok("loopsimplify")) return 0;

    LoopRotate(module.get()).run();
    if (ok("looprotate")) return 0;

    LCSSA(module.get()).run();
    if (ok("lcssa")) return 0;

// ======== Unroll ========

    if (LoopUnroll(module.get()).run()) runCleanup(module.get());
    if (ok("loopunroll")) return 0;

    if (DeadLoopElim(module.get()).run()) {
        SimplifyCFG(module.get()).run();
        DCE(module.get()).run();
    }
    if (ok("predle")) return 0;

// ======== LoopStrengthReduce ========
//    if (LoopGVN(module.get()).run()) {
//        runCleanup(module.get());
//        if (DeadLoopElim(module.get()).run()) {
//            SimplifyCFG(module.get()).run();
//            DCE(module.get()).run();
//        }
//    }
//    if (ok("loopgvn")) return 0;

    bool lsrChanged = LoopStrengthReduce(module.get()).run();
    if (ok("lsr")) return 0;
    if (lsrChanged) runCleanup(module.get());

// ======== LoopExitFold + LICM + LoopMemPromote + SubloopHoist (fixpoint) ========

    {
        bool licmChanged = false;
        int licmIter = 0;
        bool anyChanged;
        do {
            anyChanged = false;
            anyChanged |= LoopExitFold(module.get()).run();
            anyChanged |= LICM(module.get()).run();
            anyChanged |= LoopMemPromote(module.get()).run();
            anyChanged |= SubloopHoist(module.get()).run();
            if (anyChanged) {
                ++licmIter;
                licmChanged = true;
                if (okIter("licm", licmIter, "only")) return 0;
                if (ok("onlylicm")) return 0;

                bool c = true;
                while (c) {
                    c = false;
                    c |= ConstantFold(module.get()).run();
                    if (okIter("licm", licmIter, "constantfold")) return 0;
                    c |= CSE(module.get()).run();
                    if (okIter("licm", licmIter, "cse")) return 0;
                    c |= GVN(module.get()).run();
                    if (okIter("licm", licmIter, "gvn")) return 0;
                    c |= GVNHoist(module.get()).run();
                    if (okIter("licm", licmIter, "gvnhoist")) return 0;
                    c |= InstSimplify(module.get()).run();
                    if (okIter("licm", licmIter, "instsimplify")) return 0;
                    c |= SimplifyCFG(module.get()).run();
                    if (okIter("licm", licmIter, "simplifycfg")) return 0;
                    c |= DSE(module.get()).run();
                    if (okIter("licm", licmIter, "dse")) return 0;
                    c |= DCE(module.get()).run();
                    if (okIter("licm", licmIter, "dce")) return 0;
                }
            }
        } while (anyChanged);
        if (!licmChanged && okIter("licm", 1, "only")) return 0;
        if (!licmChanged && ok("onlylicm")) return 0;
    }
    if (ok("licm")) return 0;

    if (DeadLoopElim(module.get()).run()) {
        SimplifyCFG(module.get()).run();
        DCE(module.get()).run();
    }
    if (ok("postdle")) return 0;

    // All LCSSA-dependent loop passes are done.
    if (SimplifyCFG(module.get()).foldTrivialPhis()) {
        GVN(module.get()).run();
        DCE(module.get()).run();
    }

    if (dumpLIR) {
        std::cout << module->print();
        return 0;
    }

    if (!dumpAfterPass.empty()) {
        std::cerr << "Unknown dump pass: " << dumpAfterPass << "\n";
        return 1;
    }

// ======== Lowering and Backend ========

    InstSelPass isel;
    auto mcFuncs = isel.run(module.get());

    MCInvariantHoistPass mcInvariantHoist;
    for (auto& mcFunc : mcFuncs) {
        mcInvariantHoist.run(mcFunc.get());
    }

    RegAlloc regalloc;
    for (auto& mcFunc : mcFuncs) {
        regalloc.run(mcFunc.get());
    }

    MCPeepholePass peephole;
    for (auto& mcFunc : mcFuncs) {
        peephole.run(mcFunc.get());
    }
    if (!dumpHIR && !dumpLIR) {
        // Emit final assembly via AsmPrinter (handles .text / .data / .bss).
        AsmPrinter printer;
        if (!outputFile.empty()) {
            std::ofstream ofs(outputFile);
            if (!ofs.is_open()) {
                std::cerr << "Error: Could not open output file " << outputFile << "\n";
                return 1;
            }
            printer.run(mcFuncs, module.get(), ofs);
        } else {
            printer.run(mcFuncs, module.get(), std::cout);
        }
    }

    return 0;
}
