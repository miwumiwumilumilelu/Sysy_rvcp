#include "Lex/Lexer.h"
#include "Basic/Token.h"
#include <iostream>
#include <iomanip>
#include <string>

using namespace sysy;

int main() {
    std::string code = 
        "@ %#$@@! "
        "  /* Testing various numeric constants */\n"
        "const float PI = 3.14159, scale = 0x1.8p+1;\n"
        "int main() {\n"
        "    int dec = 123, oct = 0123, hex = 0xABC;\n"
        "    float f1 = 1.2e+3, f2 = .5, f3 = 5e-2;\n"
        "    float f_hex = 0x.Ap-2; \n"
        "    if (dec >= oct && hex != 0) {\n"
        "        return dec % 2;\n"
        "    }\n"
        "    return 0; // End of program\n"
        "}";
    

    std::cout << "--- Lexer : ---" << std::endl;
    std::cout << std::left << std::setw(15) << "TokenKind" 
              << std::setw(15) << "Text" 
              << "Location" << std::endl;
    std::cout << std::string(50, '-') << std::endl;

    Lexer lexer(code);
    Token tok;

    do {
        tok = lexer.nextToken();
        
        std::string kindName = tok::getTokenName(tok.getKind());
        
        std::cout << std::left << std::setw(15) << kindName 
                  << std::setw(15) << (tok.getKind() == tok::eof ? "" : std::string(tok.getText())) 
                  << "Line " << tok.getLine() << ", Col " << tok.getColumn() 
                  << std::endl;

    } while (tok.isNot(tok::eof));

    std::cout << "--- TEST PASSED ---" << std::endl;
    return 0;
}