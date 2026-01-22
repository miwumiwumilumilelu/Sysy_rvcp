#include "Lex/Lexer.h"
#include "Basic/Token.h"
#include <iostream>
#include <iomanip>
#include <string>

using namespace sysy;

int main() {
    std::string code = 
        "// This is a global constant statement\n"
        "const float PI = 3.14159;\n"
        "int main() {\n"
        "    int a = 0x12, b = 010;\n"
        "    float c = 1.23;\n"
        "    if (a >= b && !0) {\n"
        "        return a % 2;\n"
        "    }\n"
        "    return 0;\n"
        "}";

    std::cout << "--- Lexer : ---" << std::endl;
    std::cout << std::left << std::setw(15) << "TokenKind" 
              << std::setw(15) << "Text" 
              << "Location" << std::endl;
    std::cout << std::string(45, '-') << std::endl;

    Lexer lexer(code);
    Token tok;

    do {
        tok = lexer.nextToken();
        
        std::string kindName = tok::getTokenName(tok.getKind());
        
        std::cout << std::left << std::setw(15) << kindName 
                  << std::setw(15) << tok.getText() 
                  << "Line " << tok.getLine() << ", Col " << tok.getColumn() 
                  << std::endl;

    } while (tok.isNot(tok::eof));

    std::cout << "--- TEST PASSED ---" << std::endl;
    return 0;
}