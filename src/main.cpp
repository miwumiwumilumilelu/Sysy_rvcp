#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Semant/Semant.h"
#include "IR/IRGen.h"
#include "Optimize/FlattenCFG.h"
#include "CodeGen/RVGen.h"
#include <iostream>
#include <fstream>
#include <vector>

using namespace sysy;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <filename>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return 1;
    }

    // Read file content
    std::string sourceCode((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    file.close();

    std::cout << "--- Compiling " << filename << " ---" << std::endl;

    // 1. Frontend: Lexer & Parser
    Lexer lexer(sourceCode);
    Parser parser(lexer);
    auto ast = parser.parseCompUnit();

    if (!ast) {
        std::cerr << "Parser Error: Failed to generate AST." << std::endl;
        return 1;
    }

    // 2. Semantic Analysis
    Semant semant;
    ast->accept(semant);

    // 3. IR Generation (High-Level IR)
    IRGen irGen;
    ast->accept(irGen);
    auto module = irGen.getModule();

    std::cout << "\n=== Generated High-Level IR ===\n" << std::endl;
    if (module) {
        std::cout << module->print() << std::endl;
    }

    // 4. Optimization: FlattenCFG Pass (High-Level -> Low-Level)
    if (module) {
        std::cout << "\n[Running Pass] FlattenCFG..." << std::endl;
        FlattenCFG flatten(module.get());
        flatten.run();
        
        std::cout << "\n=== Flattened IR (Low-Level) ===\n" << std::endl;
        std::cout << module->print() << std::endl;
    }

    // 5. Backend: RISC-V Generation
    std::cout << "\n=== Generated RISC-V Assembly ===\n" << std::endl;
    if (module) {
        RVGen rvGen(module.get());
        rvGen.generate();
        
        // Print to console
        std::cout << rvGen.getAssembly() << std::endl;
        
        // Save to .s file
        std::string outFilename = filename + ".s";
        std::ofstream outFile(outFilename);
        if (outFile.is_open()) {
            outFile << rvGen.getAssembly();
            outFile.close();
            std::cout << "[Info] Assembly saved to: " << outFilename << std::endl;
        } else {
            std::cerr << "[Error] Could not save assembly file." << std::endl;
        }
    }

    return 0;
}