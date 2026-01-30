#ifndef PARSER_H
#define PARSER_H

#include "Lex/Lexer.h"
#include "AST/ASTNode.h"

namespace sysy {

class Parser {
    Lexer &L;
    Token CurTok;

public:
    Parser(Lexer &lexer) : L(lexer) {
        getNextToken();
    }

    std::unique_ptr<CompUnitAST> parseCompUnit();

private:
    void getNextToken() { CurTok = L.nextToken(); }
    
    // Matches and consumes a specified type of Token, 
    // and throws an error if the type is not matched.
    // (similar to Clang's ExpectAndConsume)
    bool expect(tok::TokenKind K);

    std::unique_ptr<FuncDefAST> parseFuncDef();
    std::string parseType();

    std::unique_ptr<BlockAST> parseBlock();       // {...}
    std::unique_ptr<StmtAST> parseStmt();         // (return, block, etc.)
    
    std::unique_ptr<ExprAST> parseExpr();         // Expr -> AddExpr
    std::unique_ptr<ExprAST> parseAddExpr();      // AddExpr -> MulExpr { (+|-) MulExpr }
    std::unique_ptr<ExprAST> parseMulExpr();      // MulExpr -> PrimaryExpr { (*|/|%) PrimaryExpr }
    std::unique_ptr<ExprAST> parsePrimaryExpr();  // PrimaryExpr -> Number | (Expr)
};

}

#endif