#ifndef LEXER_H
#define LEXER_H

#include "Basic/Token.h"
#include <string_view>

namespace sysy {

class Lexer {
private:
  std::string_view Buffer; // input
  const char *CurPtr;      // Current scanning position
  int CurLine;
  int CurCol;

public:
  Lexer(std::string_view buffer) 
    : Buffer(buffer), CurPtr(buffer.data()), CurLine(1), CurCol(1) {}

  Token nextToken();

private:
  void skipWhitespace();
  void skipComment();      // Handle // and /* */
  char peek() const { return *CurPtr; }
  char consume() { 
    char c = *CurPtr++; 
    if (c == '\n') { CurLine++; CurCol = 1; } 
    else { CurCol++; }
    return c; 
  }
};

}

#endif