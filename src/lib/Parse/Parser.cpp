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

std::unique_ptr<VarDeclAST> Parser::parseDecl() {
    std::string type = parseType();
    if (type.empty()) return nullptr;

    if (CurTok.isNot(tok::identifier)) {
        std::cerr << "Error: Expected variable name after type" << std::endl;
        return nullptr;
    }
    std::string name(CurTok.getText());
    getNextToken();

    std::unique_ptr<ExprAST> init = nullptr;
    if (CurTok.is(tok::equal)) {
        getNextToken();
        init = parseExpr();
    }

    if (!expect(tok::semi)) return nullptr;

    return std::make_unique<VarDeclAST>(type, name, std::move(init));
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
    else if (CurTok.is(tok::identifier)) {
        std::string name(CurTok.getText());
        getNextToken();
        return std::make_unique<LValAST>(name);
    }

    std::cerr << "Error: Unexpected token in expression: " << CurTok.getText() << std::endl;
    return nullptr;
}

std::unique_ptr<ExprAST> Parser::parseUnaryExpr() {
    if (CurTok.is(tok::plus) || CurTok.is(tok::minus) || CurTok.is(tok::exclaim)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto operand = parseUnaryExpr();
        if (!operand) return nullptr;
        return std::make_unique<UnaryExprAST>(op, std::move(operand));
    }
    return parsePrimaryExpr();
}

std::unique_ptr<ExprAST> Parser::parseMulExpr() {
    auto lhs = parsePrimaryExpr();
    if (!lhs) return nullptr;

    while (CurTok.is(tok::star) || CurTok.is(tok::slash) || CurTok.is(tok::percent)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto rhs = parseUnaryExpr();
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

std::unique_ptr<ExprAST> Parser::parseRelExpr() {
    auto lhs = parseAddExpr();
    if (!lhs) return nullptr;
    while (CurTok.is(tok::less) || CurTok.is(tok::greater) || 
           CurTok.is(tok::less_equal) || CurTok.is(tok::greater_equal)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto rhs = parseAddExpr();
        if (!rhs) return nullptr;
        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExprAST> Parser::parseEqExpr() {
    auto lhs = parseRelExpr();
    if (!lhs) return nullptr;
    while (CurTok.is(tok::equal_equal) || CurTok.is(tok::not_equal)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto rhs = parseRelExpr();
        if (!rhs) return nullptr;
        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExprAST> Parser::parseLogicAndExpr() {
    auto lhs = parseEqExpr();
    if (!lhs) return nullptr;
    while (CurTok.is(tok::amp_amp)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto rhs = parseEqExpr();
        if (!rhs) return nullptr;
        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExprAST> Parser::parseLogicOrExpr() {
    auto lhs = parseLogicAndExpr();
    if (!lhs) return nullptr;
    while (CurTok.is(tok::pipe_pipe)) {
        std::string op(CurTok.getText());
        getNextToken();
        auto rhs = parseLogicAndExpr();
        if (!rhs) return nullptr;
        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExprAST> Parser::parseExpr() {
    return parseLogicOrExpr();
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
    else if (CurTok.is(tok::kw_if)) {
        getNextToken(); // consume 'if'
        expect(tok::l_paren);
        auto cond = parseExpr();
        expect(tok::r_paren);
        auto thenStmt = parseStmt();
        std::unique_ptr<StmtAST> elseStmt = nullptr;
        if (CurTok.is(tok::kw_else)) {
            getNextToken(); // consume 'else'
            elseStmt = parseStmt();
        }
        return std::make_unique<IfStmtAST>(std::move(cond), std::move(thenStmt), std::move(elseStmt));
    }
    else if (CurTok.is(tok::kw_while)) {
        getNextToken(); // consume 'while'
        expect(tok::l_paren);
        auto cond = parseExpr();
        expect(tok::r_paren);
        auto body = parseStmt();
        return std::make_unique<WhileStmtAST>(std::move(cond), std::move(body));
    }
    else if (CurTok.is(tok::identifier)) {
        auto expr = parseExpr();
        if (CurTok.is(tok::equal)) {
            LValAST *lval = dynamic_cast<LValAST *>(expr.get());
            if (!lval) {
                std::cerr << "Error: Left side of assignment must be a variable." << std::endl;
                return nullptr;
            }
            auto lvalNode = std::make_unique<LValAST>(lval->getName());

            getNextToken(); // consume '='
            auto val = parseExpr();
            if (!expect(tok::semi)) return nullptr;
            return std::make_unique<AssignStmtAST>(std::move(lvalNode), std::move(val));
        } else {
            if (!expect(tok::semi)) return nullptr;
            return std::make_unique<ExprStmtAST>(std::move(expr));
        }
    }
    else if (CurTok.isNot(tok::r_brace) && CurTok.isNot(tok::semi)) {
        auto expr = parseExpr();
        if (!expect(tok::semi)) return nullptr;
        return std::make_unique<ExprStmtAST>(std::move(expr));
    }
    else if (CurTok.is(tok::semi)) {
        getNextToken(); // consume ';'
        return nullptr;
    }

    return nullptr;
}

std::unique_ptr<BlockAST> Parser::parseBlock() {
    if (!expect(tok::l_brace)) return nullptr;

    auto block = std::make_unique<BlockAST>();

    while (CurTok.isNot(tok::r_brace) && CurTok.isNot(tok::eof)) {
        if (CurTok.is(tok::kw_int) || CurTok.is(tok::kw_float)) {
            if (auto decl = parseDecl()) {
                block->addItem(std::move(decl));
            } 
        } else {
            if (auto stmt = parseStmt()) {
                block->addItem(std::move(stmt));
            } else {
                getNextToken(); // Skip Error.
            }
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
            getNextToken(); 
        }
    }
    return unit;
}
