#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include <iostream>
#include <string>

using namespace sysy;

int main() {
    std::string code = 
        "int main() {\n"
        "    float compute() {};\n"
        "    return 0;\n"
        "}\n"
        "float compute() {}";
    

    std::cout << "--- Starting Syntax Analysis ---" << std::endl;

    Lexer lexer(code);
    Parser parser(lexer);

    auto ast = parser.parseCompUnit();

    if (ast) {
        std::cout << "\n--- Generated AST Dump ---" << std::endl;
        ast->dump(0);
    } else {
        std::cerr << "Parsing failed!" << std::endl;
    }

    std::cout << "\n--- TEST COMPLETED ---" << std::endl;
    return 0;
}