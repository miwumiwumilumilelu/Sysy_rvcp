#include "Lex/Lexer.h"
#include <cctype>
#include <map>
#include <iostream>

using namespace sysy;

void Lexer::skipWhitespace() {
    while (CurPtr < Buffer.end() && std::isspace(*CurPtr)) {
        consume();
    }
}

void Lexer::skipComment() {
    if (peek() == '/') {
        if (CurPtr + 1 < Buffer.end() && *(CurPtr + 1) == '/') {
            while (CurPtr < Buffer.end() && peek() != '\n') consume();
        } else if (CurPtr + 1 < Buffer.end() && *(CurPtr + 1) == '*') {
            consume(); consume();
            while (CurPtr < Buffer.end()) {
                if (peek() == '*' && (CurPtr + 1 < Buffer.end()) && *(CurPtr + 1) == '/') {
                    consume(); consume();
                    return;
                }
                consume();
            }
            // Without finding */ before EOF.
            std::cerr << "Lexical Error: Unterminated multi-line comment starting at Line " << CurLine << std::endl;
        }
    }
}

Token Lexer::lexNumericConstant() {
    const char *StartPtr = CurPtr;
    Token Result;
    Result.setLocation(CurLine, CurCol);

    bool isFloat = false;

    // Check for hex prefixes 0x or 0X.
    if (peek() == '0' && (CurPtr + 1 < Buffer.end()) && 
    (std::tolower(*(CurPtr + 1)) == 'x')) {
        consume(); // '0'
        consume(); // 'x' or 'X'

        // Scan hexadecimal digits or decimal point.
        while (std::isxdigit(peek()) || peek() == '.') {
            if (consume() == '.') isFloat = true;
        }

        // Hexadecimal floating-point specific exponent part: p or P.
        if (std::tolower(peek()) == 'p') {
            isFloat = true;
            consume(); // 'p'
            if (peek() == '+' || peek() == '-') consume();
            while (std::isdigit(peek())) consume();
        }
    } 

  // Decimal or octal.
    else {
        while (std::isdigit(peek()) || peek() == '.') {
            if (consume() == '.') isFloat = true;
        }

        // Decimal floating-point scientific notation: e or E.
        if (std::tolower(peek()) == 'e') {
            isFloat = true;
            consume(); // 'e'
            if (peek() == '+' || peek() == '-') consume();
            while (std::isdigit(peek())) consume();
        }
    }

    Result.setText(std::string_view(StartPtr, CurPtr - StartPtr));
    
    Result.setKind(isFloat ? tok::float_const : tok::int_const);
    return Result;
}

Token Lexer::nextToken() {
    skipWhitespace();
  
    while (CurPtr < Buffer.end() && peek() == '/') {
        if (CurPtr + 1 < Buffer.end() && (*(CurPtr + 1) == '/' || *(CurPtr + 1) == '*')) {
            skipComment();
            skipWhitespace();
        } else break;
    }

    Token Result;
    Result.setLocation(CurLine, CurCol);

    if (CurPtr >= Buffer.end()) {
        Result.setKind(tok::eof);
        return Result;
    }

    char c = peek();
    const char *StartPtr = CurPtr;

    if (std::isalpha(c) || c == '_') {
        consume();
        while (std::isalnum(peek()) || peek() == '_') consume();
        
        std::string_view Text(StartPtr, CurPtr - StartPtr);
        Result.setText(Text);

        tok::TokenKind Kind = tok::identifier;
        #define KEYWORD(X) if (Text == #X) Kind = tok::kw_##X; else
        #include "Basic/TokenKinds.def"
        { Kind = tok::identifier; }
        
        Result.setKind(Kind);
        return Result;
    }

    if (std::isdigit(c) || (c == '.' && std::isdigit(*(CurPtr + 1)))) {
        return lexNumericConstant();
    }

    std::string_view SingleChar(StartPtr, 1);
    std::string_view DoubleChar = SingleChar;
    // Prevent out-of-bounds access.
    if (CurPtr < Buffer.end()) {
        DoubleChar = std::string_view(StartPtr, 2);
    }

    #define PUNCTUATOR(X, Y) \
        if (std::string_view(Y).size() == 2 && DoubleChar == Y) { \
            consume(); consume(); \
            Result.setKind(tok::X); Result.setText(DoubleChar); return Result; \
        }
    #include "Basic/TokenKinds.def"

    #define PUNCTUATOR(X, Y) \
            if (std::string_view(Y).size() == 1 && SingleChar == Y) { \
                consume(); \
                Result.setKind(tok::X); Result.setText(SingleChar); return Result; \
            }
    #include "Basic/TokenKinds.def"

    std::cerr << "Lexical Error at (Line: " << Result.getLine() 
            << ", Col: " << Result.getColumn() << "): Unknown character '" 
            << c << "'" << std::endl;
    Result.setText(std::string_view(CurPtr, 1));
    consume();
    Result.setKind(tok::unknown);
    return Result;
}