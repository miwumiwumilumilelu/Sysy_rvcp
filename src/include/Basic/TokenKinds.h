#ifndef TOKENKINDS_H
#define TOKENKINDS_H

namespace sysy {
namespace tok {

enum TokenKind : unsigned short {
#define TOK(X) X,
#include "Basic/TokenKinds.def"
  NUM_TOKENS
};

/// Get the internal name of the Token for debugging
const char *getTokenName(TokenKind Kind);

const char *getPunctuatorSpelling(TokenKind Kind);

const char *getKeywordSpelling(TokenKind Kind);

} // sysy
} // tok

#endif