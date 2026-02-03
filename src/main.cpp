#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Semant/Semant.h"
#include "IR/IRGen.h"
#include <iostream>
#include <string>

using namespace sysy;

int main() {
    std::string code = 
        "int main() {\n"
        "    int a = 10;\n"
        "    int b = 5;\n"
        "\n"
        "    if (a > b) {\n"
        "        a = a - 1;\n"
        "    } else {\n"
        "        b = b + 1;\n"
        "    }\n"
        "\n"
        "    while (b > 0) {\n"
        "        b = b - 1;\n"
        "    }\n"
        "\n"
        "    return a;\n"
        "}\n";

    std::cout << "--- Starting Compilation ---" << std::endl;

    Lexer lexer(code);
    Parser parser(lexer);

    // 1. Parsing
    auto ast = parser.parseCompUnit();
    if (!ast) {
        std::cerr << "Parsing failed!" << std::endl;
        return 1;
    }

    // 2. Semantic Analysis
    std::cout << "\n[Semantic Analysis]..." << std::endl;
    Semant semant;
    ast->accept(semant);

    // 3. IR Generation (Object-based)
    std::cout << "\n[IR Generation]..." << std::endl;
    IRGen irGen;
    ast->accept(irGen);
    
    // Get Module
    auto module = irGen.getModule();
    
    // Print IR
    std::cout << "\n=== Generated IR ===\n" << std::endl;
    if (module) {
        std::cout << module->print() << std::endl;
    }

    std::cout << "\n--- TEST COMPLETED ---" << std::endl;
    return 0;
}