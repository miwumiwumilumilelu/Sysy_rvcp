#include "Lex/Lexer.h"
#include "Parse/Parser.h"
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