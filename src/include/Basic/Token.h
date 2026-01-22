#ifndef TOKEN_H
#define TOKEN_H

#include "Basic/TokenKinds.h"
#include <string>
#include <string_view>

namespace sysy {

class Token {
private:
  tok::TokenKind Kind;
  // Fragment pointing to the source code buffer to avoid frequent string copying
  std::string_view Text;
  int Line;
  int Column;

public:
  Token() : Kind(tok::unknown), Line(0), Column(0) {}

  void setKind(tok::TokenKind k) { Kind = k; }
  tok::TokenKind getKind() const { return Kind; }

  void setText(std::string_view text) { Text = text; }
  std::string_view getText() const { return Text; }

  void setLocation(int line, int col) { Line = line; Column = col; }
  int getLine() const { return Line; }
  int getColumn() const { return Column; }

  bool is(tok::TokenKind k) const { return Kind == k; }
  bool isNot(tok::TokenKind k) const { return Kind != k; }
};

}

#endif