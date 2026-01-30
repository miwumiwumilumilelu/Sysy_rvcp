#include "Parse/Parser.h"
#include <iostream>

using namespace sysy;

bool Parser::expect(tok::TokenKind K) {
    if (CurTok.is(K)) {
        getNextToken();
        return true;
    }
    std::cerr << "Parser Error: Expected '" << tok::getTokenName(K) 
              << "' but found '" << tok::getTokenName(CurTok.getKind()) 
              << "' at Line " << CurTok.getLine() << ", Col " << CurTok.getColumn() << std::endl;
    return false;
}

std::string Parser::parseType() {
    std::string typeStr;
    if (CurTok.is(tok::kw_int)) typeStr = "int";
    else if (CurTok.is(tok::kw_float)) typeStr = "float";
    else if (CurTok.is(tok::kw_void)) typeStr = "void";
    else return "";

    getNextToken();
    return typeStr;
}

std::unique_ptr<ExprAST> Parser:: parsePrimaryExpr() {
    if (CurTok.is(tok::int_const)) {
        int val = std::stoi(std::string(CurTok.getText()));
        getNextToken();
        return std::make_unique<NumberAST>(val);
    }
    else if (CurTok.is(tok::float_const)) {
        float val = std::stof(std::string(CurTok.getText()));
        getNextToken();
        return std::make_unique<NumberAST>(val);
    }
    else if (CurTok.is(tok::l_paren)) {
        auto expr = parseExpr();
        if (!expr) return nullptr;
        if (!expect(tok::r_paren)) return nullptr;
        return expr;
    }

    std::cerr << "Error: Unexpected token in expression: " << CurTok.getText() << std::endl;
    return nullptr;
}

std::unique_ptr<ExprAST> Parser::parseMulExpr() {
    auto lhs = parsePrimaryExpr();
    if (!lhs) return nullptr;

    while (CurTok.is(tok::star) || CurTok.is(tok::slash) || CurTok.is(tok::percent)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto rhs = parsePrimaryExpr();
        if (!rhs) return nullptr;
        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExprAST> Parser::Parser::parseAddExpr() {
    auto lhs = parseMulExpr();
    if (!lhs) return nullptr;

    while (CurTok.is(tok::plus) || CurTok.is(tok::minus)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto rhs = parseMulExpr();
        if (!rhs) return nullptr;
        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExprAST> Parser::parseExpr() {
    return parseAddExpr();
}

std::unique_ptr<StmtAST> Parser::parseStmt() {
    if (CurTok.is(tok::kw_return)) {
        getNextToken(); // consume 'return'

        std::unique_ptr<ExprAST> val = nullptr;
        if (CurTok.isNot(tok::semi)) {
            val = parseExpr();
        }

        if (!expect(tok::semi)) return nullptr;
        return std::make_unique<ReturnStmtAST>(std::move(val));
    }
    else if (CurTok.is(tok::l_brace)) {
        return parseBlock();
    }

    // If it doesn't match return or block for the time being, we consider it an empty statement or an error, skip a token to avoid infinite loops.
    // Later, this will be extended to include ExprStmt (expression statement) and Decl (declaration).

    if (CurTok.isNot(tok::r_brace) && CurTok.isNot(tok::eof)) {
        std::cerr << "Warning: Skipping unrecognized statement token: " << CurTok.getText() << std::endl;
        getNextToken();
    }
    return nullptr;
}

std::unique_ptr<BlockAST> Parser::parseBlock() {
    if (!expect(tok::l_brace)) return nullptr;

    auto block = std::make_unique<BlockAST>();

    // Here it needs to be judged whether it is Declaration or Statement.
    // Currently simplified: Assume BlockItem are all Statement.
    while (CurTok.isNot(tok::r_brace) && CurTok.isNot(tok::eof)) {
        if (auto stmt = parseStmt()) {
            block->addItem(std::move(stmt));
        } 
    }

    if (!expect(tok::r_brace)) return nullptr;
    return block;
}

std::unique_ptr<FuncDefAST> Parser::parseFuncDef() {
std::string retType = parseType();
    if (retType.empty()) return nullptr;

    if (CurTok.isNot(tok::identifier)) {
        std::cerr << "Error: Expected function name after type" << std::endl;
        return nullptr;
    }
    std::string name(CurTok.getText());
    getNextToken();

    if (!expect(tok::l_paren)) return nullptr;
    if (!expect(tok::r_paren)) return nullptr;

    auto body = parseBlock();
    if (!body) return nullptr;

    return std::make_unique<FuncDefAST>(name, retType, std::move(body));
}

std::unique_ptr<CompUnitAST> Parser::parseCompUnit() {
    auto unit = std::make_unique<CompUnitAST>();
    while (CurTok.isNot(tok::eof)) {
        if (auto func = parseFuncDef()) {
            unit->addChild(std::move(func));
        } else {
            if (CurTok.is(tok::eof)) break;
            getNextToken(); 
        }
    }
    return unit;
}
