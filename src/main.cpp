#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Semant/Semant.h"
#include <iostream>
#include <string>

using namespace sysy;

int main() {
    std::string code = 
        "int main() {\n"
        "    int a = 10;\n"
        "    int b = 5;\n"
        "    if (a > b) {\n"
        "        int temp = a;\n"
        "        a = b;\n"
        "        b = temp;\n"
        "    }\n"
        "    b = a + 1;\n"
        "    return 0;\n"
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

    std::cout << "\n[AST Dump]" << std::endl;
    ast->dump(0);

    // 2. Semantic Analysis
    std::cout << "\n[Semantic Analysis]" << std::endl;
    Semant semant;
    ast->accept(semant);

    std::cout << "\n--- TEST COMPLETED ---" << std::endl;
    return 0;
}