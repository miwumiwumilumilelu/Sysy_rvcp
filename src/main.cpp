#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Semant/Semant.h"
#include "IR/IRGen.h"
#include "Optimize/CFG/FlattenCFG.h"
#include "Optimize/High/HighMem2Reg.h"
#include "Optimize/High/HighLICM.h"
#include "Optimize/Analysis/Dominators.h"
#include "Optimize/Scalar/Mem2Reg.h"
#include "Optimize/Scalar/ConstantFold.h"
#include "Optimize/Scalar/CSE.h"
#include "Optimize/Scalar/GVN.h"
#include "Optimize/Scalar/GVNHoist.h"
#include "Optimize/Scalar/InstSimplify.h"
#include "Optimize/CFG/SimplifyCFG.h"
#include "Optimize/Scalar/DCE.h"
#include "Optimize/Scalar/DSE.h"
#include "Optimize/Scalar/DFE.h"
#include "Optimize/Scalar/StrengthReduce.h"
#include "Optimize/Scalar/SSAInline.h"
#include "Optimize/Scalar/ConstSpec.h"
#include "Optimize/Scalar/LoopUnroll.h"
#include "Optimize/Loop/LoopSimplify.h"
#include "Optimize/Loop/LoopRotate.h"
#include "Optimize/Loop/LCSSA.h"
#include "Optimize/Loop/DeadLoopElim.h"
#include "Optimize/Loop/LICM.h"
#include "Optimize/Loop/LoopExitFold.h"
#include "Optimize/Loop/LoopMemPromote.h"
#include "Optimize/Loop/SubloopHoist.h"
#include "Optimize/Loop/LoopStrengthReduce.h"
#include "Optimize/Loop/LoopGVN.h"
#include "rv/InstSel.h"
#include "rv/RegAlloc.h"
#include "rv/MCPeephole.h"
#include "rv/AsmPrinter.h"

using namespace sysy;
using namespace sysy::rv;

static bool runCleanup(Module* m) {
    bool anyChanged = false;
    bool c = true;
    while (c) {
        c = false;
        c |= ConstantFold(m).run();
        c |= CSE(m).run();
        c |= GVN(m).run();
        c |= GVNHoist(m).run();
        c |= InstSimplify(m).run();
        c |= SimplifyCFG(m).run();
        c |= DSE(m).run();
        c |= DCE(m).run();
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
        } else if (arg == "--dump-mid-ir") {
            dumpHIR = true;
        } else if (arg == "--dump-cfg-ir") {
            dumpLIR = true;
        } else if (arg.rfind("--dump-", 0) == 0 && arg.size() > 10 &&
                   arg.substr(arg.size() - 3) == "-ir") {
            dumpAfterPass = arg.substr(7, arg.size() - 10);
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
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

    HighMem2Reg highMem2Reg(module.get());
    highMem2Reg.run();
    if (ok("high-mem2reg")) return 0;

    HighLICM highLICM(module.get());
    highLICM.run();
    if (ok("high-licm")) return 0;

// ======== Flattened CFG ========

    FlattenCFG flatten(module.get());
    flatten.run();
    if (ok("flatten-cfg")) return 0;

// ======== Mem2Reg ========

    Mem2Reg mem2reg(module.get(), nullptr);
    mem2reg.run();
    if (ok("mem2reg")) return 0;

// ======== Scalar Cleanup ========

    runCleanup(module.get());
    if (ok("scalar-cleanup")) return 0;

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
    // if (ok("const-spec")) return 0;

// ======== Post-ConstSpec Inline ========

    while (SSAInline(module.get()).run()) {
        runCleanup(module.get());
    }
    if (ok("post-spec-inline")) return 0;

// ======== Dead Function Elimination ========

    DFE(module.get()).run();
    if (ok("dfe")) return 0;

// ======== StrengthReduce ========

    if (StrengthReduce(module.get()).run()) {
        runCleanup(module.get());
    }
    if (ok("strength-reduce")) return 0;

// ======== Loop Canonicalization ========

    LoopSimplify(module.get()).run();
    if (ok("loop-simplify")) return 0;

    LoopRotate(module.get()).run();
    if (ok("loop-rotate")) return 0;

    LCSSA(module.get()).run();
    if (ok("lcssa")) return 0;

// ======== Unroll ========

    if (LoopUnroll(module.get()).run()) {
        runCleanup(module.get());
    }
    if (ok("loop-unroll")) return 0;

    if (DeadLoopElim(module.get()).run()) {
        SimplifyCFG(module.get()).run();
        DCE(module.get()).run();
    }
    if (ok("deadloop-elim-pre-licm")) return 0;

// ======== LoopGVN ========
//    if (LoopGVN(module.get()).run()) {
//        runCleanup(module.get());
//        if (DeadLoopElim(module.get()).run()) {
//            SimplifyCFG(module.get()).run();
//            DCE(module.get()).run();
//        }
//    }
//     if (ok("loop-gvn")) return 0;

    LoopStrengthReduce(module.get()).run();
    if (ok("loop-strength-reduce")) return 0;

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
                if (ok("licm-only")) return 0;

                bool c = true;
                while (c) {
                    c = false;
                    c |= ConstantFold(module.get()).run();
                    if (okIter("licm", licmIter, "cf")) return 0;
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
        if (!licmChanged && ok("licm-only")) return 0;
    }
    if (ok("licm")) return 0;

    if (DeadLoopElim(module.get()).run()) {
        SimplifyCFG(module.get()).run();
        DCE(module.get()).run();
    }
    if (ok("deadloop-elim-post-licm")) return 0;

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
