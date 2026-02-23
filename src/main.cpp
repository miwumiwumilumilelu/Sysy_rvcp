#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Semant/Semant.h"
#include "IR/IRGen.h"
#include "Optimize/FlattenCFG.h"
#include "Optimize/Dominators.h"
#include "Optimize/Mem2Reg.h"
#include "Optimize/ConstantFold.h"
#include "Optimize/SimplifyCFG.h"
#include "Optimize/DCE.h"
#include "rv/InstSelector.h"
#include "rv/MCPrinter.h"
#include "rv/PhiElim.h"
#include "rv/RegAlloc.h"

using namespace sysy;

int main(int argc, char **argv) {
    std::string inputFile;
    std::string outputFile;
    bool dumpHIR = false;
    bool dumpLIR = false;

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

    if (outputFile.empty()) {
        outputFile = inputFile + ".s";
    }

    std::ifstream file(inputFile);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << inputFile << "\n";
        return 1;
    }
    std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

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

    if (dumpHIR) {
        std::cout << module->print();
    }

    std::cerr << "[Debug] Running FlattenCFG..." << std::endl;
    FlattenCFG flatten(module.get());
    flatten.run();

    std::cerr << "[Debug] Running Mem2Reg (Dominators inside)..." << std::endl;
    Mem2Reg mem2reg(module.get(), nullptr); 
    mem2reg.run();

    for (int i = 0; i < 3; i++) {
        ConstantFold constFold(module.get());
        constFold.run();
        
        SimplifyCFG simplify(module.get());
        simplify.run();

        DCE dce(module.get());
        dce.run();
    }

    if (dumpLIR) {
        std::cout << module->print();
    }

    std::cerr << "\n[Debug] ----- Starting Instruction Selection -----" << std::endl;
    InstSelector selector;
    MCModule* machineModule = selector.run(module.get());

    std::cerr << "\n[Debug] ----- Running Phi Elimination -----" << std::endl;
    PhiElim pe;
    pe.run(machineModule);

    std::cerr << "\n[Debug] ----- Running Register Allocation -----" << std::endl;
    RegAlloc allocator;
    allocator.run(machineModule);

    std::cerr << "\n[Debug] ----- Machine IR (Virtual Assembly) -----\n" << std::endl;
    MCPrinter printer;
    if (!dumpLIR && !dumpHIR) {
        printer.print(machineModule, std::cout);
    }
    


    return 0;
}