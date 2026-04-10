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
#include "Optimize/Loop/LoopSimplify.h"
#include "Optimize/Loop/LoopRotate.h"
#include "Optimize/Loop/LCSSA.h"
#include "Optimize/Loop/LICM.h"
#include "rv/InstSel.h"
#include "rv/RegAlloc.h"
#include "rv/MCPeephole.h"
#include "rv/AsmPrinter.h"

using namespace sysy;
using namespace sysy::rv;

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

    bool changed = true;
    while (changed) {
        changed = false;
        changed |= ConstantFold(module.get()).run();
        changed |= CSE(module.get()).run();
        changed |= GVN(module.get()).run();
        changed |= GVNHoist(module.get()).run();
        changed |= InstSimplify(module.get()).run();
        changed |= SimplifyCFG(module.get()).run();
        changed |= DCE(module.get()).run();
    }
    if (ok("scalar-cleanup")) return 0;

// ======== Loop Optimization ========

    LoopSimplify loopSimplify(module.get());
    loopSimplify.run();
    if (ok("loop-simplify")) return 0;

    LoopRotate loopRotate(module.get());
    loopRotate.run();
    if (ok("loop-rotate")) return 0;

    LCSSA lcssa(module.get());
    lcssa.run();
    if (ok("lcssa")) return 0;

    while (LICM(module.get()).run()) {
        bool c = true;
        while (c) {
            c = false;
            c |= ConstantFold(module.get()).run();
            c |= CSE(module.get()).run();
            c |= GVN(module.get()).run();
            c |= GVNHoist(module.get()).run();
            c |= InstSimplify(module.get()).run();
            c |= SimplifyCFG(module.get()).run();
            c |= DCE(module.get()).run();
        }
    }
    if (ok("licm")) return 0;

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
