#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Semant/Semant.h"
#include "IR/IRGen.h"
// #include "CodeGen/RISCVGen.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

using namespace sysy;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <filename.sy>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    std::ifstream t(filename);
    if (!t.is_open()) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return 1;
    }
    std::stringstream buffer;
    buffer << t.rdbuf();
    std::string code = buffer.str();

    std::cout << "--- Compiling " << filename << " ---" << std::endl;

    Lexer lexer(code);
    Parser parser(lexer);

    // 1. Parsing
    auto ast = parser.parseCompUnit();
    if (!ast) {
        std::cerr << "Parsing failed!" << std::endl;
        return 1;
    }

    // 2. Semantic Analysis
    Semant semant;
    ast->accept(semant);

    // 3. IR Generation
    IRGen irGen;
    ast->accept(irGen);
    auto module = irGen.getModule();

    std::cout << "\n=== Generated IR ===" << std::endl;
    if (module) {
        std::cout << module->print() << std::endl;
    }

    // // 4. RISC-V Generation
    // std::cout << "\n=== Generated RISC-V Assembly ===" << std::endl;
    // if (module) {
    //     RISCVGen riscvGen(module.get());
    //     riscvGen.generate();
    //     std::cout << riscvGen.getAssembly() << std::endl;
        
    //     std::string asmFilename = filename + ".s";
    //     std::ofstream asmFile(asmFilename);
    //     asmFile << riscvGen.getAssembly();
    //     std::cout << "\n[Info] Assembly saved to " << asmFilename << std::endl;
    // }

    return 0;
}