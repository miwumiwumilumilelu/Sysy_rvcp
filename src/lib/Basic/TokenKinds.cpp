#include "Basic/TokenKinds.h"
#include <cassert>

using namespace sysy;

const char *tok::getTokenName(TokenKind Kind) {
  switch (Kind) {
#define TOK(X) case tok::X: return #X;
#include "Basic/TokenKinds.def"
    default: return "unknown";
  }
}

const char *tok::getPunctuatorSpelling(TokenKind Kind) {
  switch (Kind) {
#define PUNCTUATOR(X, Y) case tok::X: return Y;
#include "Basic/TokenKinds.def"
    default: return nullptr;
  }
}

const char *tok::getKeywordSpelling(TokenKind Kind) {
  switch (Kind) {
#define KEYWORD(X) case tok::kw_##X: return #X;
#include "Basic/TokenKinds.def"
    default: return nullptr;
  }
}