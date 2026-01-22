#include "Lex/Lexer.h"
#include <cctype>
#include <map>

using namespace sysy;

void Lexer::skipWhitespace() {
    while (CurPtr < Buffer.end() && std::isspace(*CurPtr)) {
        consume();
    }
}

void Lexer::skipComment() {
    if (peek() == '/') {
        if (*(CurPtr + 1) == '/') {
            consume(); consume();
            while (CurPtr < Buffer.end() && peek() != '\n') consume();
        } else if (*(CurPtr + 1) == '*') {
            consume(); consume();
            while (CurPtr < Buffer.end()) {
                if (peek() == '*' && *(CurPtr + 1) == '/') {
                    consume(); consume();
                    break;
                }
                consume();
            }
        }
    }
}

Token Lexer::nextToken() {
    skipWhitespace();
  
    while (CurPtr < Buffer.end() && peek() == '/') {
        if (*(CurPtr + 1) == '/' || *(CurPtr + 1) == '*') {
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

    if (std::isdigit(c)) {
        bool isFloat = false;
        consume();
        while (std::isalnum(peek()) || peek() == '.') {
            if (consume() == '.') isFloat = true;
        }
        
        Result.setText(std::string_view(StartPtr, CurPtr - StartPtr));
        Result.setKind(isFloat ? tok::float_const : tok::int_const);
        return Result;
    }

    consume();
    std::string_view SingleChar(StartPtr, 1);
    std::string_view DoubleChar(StartPtr, 2);

    #define PUNCTUATOR(X, Y) \
        if (std::string_view(Y).size() == 2 && DoubleChar == Y) { \
            if (std::string_view(Y).size() == 2) { consume(); } \
            Result.setKind(tok::X); Result.setText(DoubleChar); return Result; \
        }
    #include "Basic/TokenKinds.def"

    #define PUNCTUATOR(X, Y) \
            if (std::string_view(Y).size() == 1 && SingleChar == Y) { \
                Result.setKind(tok::X); Result.setText(SingleChar); return Result; \
            }
    #include "Basic/TokenKinds.def"

    Result.setKind(tok::unknown);
    return Result;
}