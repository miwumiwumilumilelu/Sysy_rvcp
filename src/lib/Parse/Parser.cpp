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

std::unique_ptr<ExprAST> Parser::parseFuncCall(const std::string &name) {
    if (!expect(tok::l_paren)) return nullptr;

    std::vector<std::unique_ptr<ExprAST>> args;

    if (CurTok.isNot(tok::r_paren)) {
        while (true) {
            auto arg = parseExpr();
            if (!arg) return nullptr;
            args.push_back(std::move(arg));

            if (CurTok.is(tok::comma)) {
                getNextToken();
            } else {
                break;
            }
        }
    }

    if (!expect(tok::r_paren)) return nullptr;
    
    return std::make_unique<FuncCallAST>(name, std::move(args));
}

std::unique_ptr<InitValAST> Parser::parseInitVal() {
    if (CurTok.is(tok::l_brace)) {
        getNextToken(); // consume '{'
        std::vector<std::unique_ptr<InitValAST>> elements;
        if (CurTok.isNot(tok::r_brace)) {
            while (true) {
                auto elem = parseInitVal();
                if (!elem) return nullptr;
                elements.push_back(std::move(elem));
                
                if (CurTok.is(tok::comma)) {
                    getNextToken();
                } else {
                    break;
                }
            }
        }
        if (!expect(tok::r_brace)) return nullptr;
        return std::make_unique<InitValAST>(std::move(elements));
    } else {
        auto expr = parseExpr();
        if (!expr) return nullptr;
        return std::make_unique<InitValAST>(std::move(expr));
    }
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

    std::vector<std::unique_ptr<ExprAST>> dims;
    while (CurTok.is(tok::l_square)) {
        getNextToken(); // consume '['
        auto dim = parseExpr();
        if (!dim) return nullptr;
        dims.push_back(std::move(dim));
        if (!expect(tok::r_square)) return nullptr;
    }

    std::unique_ptr<InitValAST> init = nullptr;
    if (CurTok.is(tok::equal)) {
        getNextToken();
        init = parseInitVal();
    }

    if (!expect(tok::semi)) return nullptr;

    return std::make_unique<VarDeclAST>(type, name, std::move(dims), std::move(init));
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
        getNextToken();

        auto expr = parseExpr();
        if (!expr) return nullptr;
        if (!expect(tok::r_paren)) return nullptr;
        return expr;
    } 
    else if (CurTok.is(tok::identifier)) {
        std::string name(CurTok.getText());
        getNextToken();

        if (CurTok.is(tok::l_paren)) {
            return parseFuncCall(name);
        } 

        std::vector<std::unique_ptr<ExprAST>> indices;
        while (CurTok.is(tok::l_square)) {
            getNextToken(); // consume '['
            auto idx = parseExpr();
            if (!idx) return nullptr;
            indices.push_back(std::move(idx));
            if (!expect(tok::r_square)) return nullptr;
        }
        
        return std::make_unique<LValAST>(name, std::move(indices));
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
    auto lhs = parseUnaryExpr();
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
        if (!cond) return nullptr;
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
        if (!cond) return nullptr;
        expect(tok::r_paren);
        auto body = parseStmt();
        return std::make_unique<WhileStmtAST>(std::move(cond), std::move(body));
    }
    else if (CurTok.is(tok::identifier)) {
        auto expr = parseExpr();
        if (CurTok.is(tok::equal)) {
            if (dynamic_cast<LValAST *>(expr.get())) {
                std::unique_ptr<LValAST> lvalNode(static_cast<LValAST*>(expr.release()));

                getNextToken();
                auto val = parseExpr();
                if (!expect(tok::semi)) return nullptr;
                return std::make_unique<AssignStmtAST>(std::move(lvalNode), std::move(val));
            } else {
                std::cerr << "Error: Left side of assignment must be a variable." << std::endl;
                return nullptr;
            }
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
    std::vector<std::unique_ptr<FuncFParamAST>> params;
    if (CurTok.isNot(tok::r_paren)) {
        while (true) {
            std::string paramType = parseType();
            if (paramType.empty()) {
                 std::cerr << "Error: Expected type in function parameter" << std::endl;
                 return nullptr;
            }

            if (CurTok.isNot(tok::identifier)) {
                std::cerr << "Error: Expected parameter name" << std::endl;
                return nullptr;
            }
            std::string paramName(CurTok.getText());
            getNextToken();
            std::vector<std::unique_ptr<ExprAST>> dims;
            while (CurTok.is(tok::l_square)) {
                getNextToken();
                
                if (CurTok.is(tok::r_square)) {
                    dims.push_back(nullptr);
                    getNextToken();
                } else {
                    auto dim = parseExpr();
                    dims.push_back(std::move(dim));
                    if (!expect(tok::r_square)) return nullptr;
                }
            }

            params.push_back(std::make_unique<FuncFParamAST>(paramType, paramName, std::move(dims)));

            if (CurTok.is(tok::comma)) {
                getNextToken();
            } else {
                break;
            }
        }
    }

    if (!expect(tok::r_paren)) return nullptr;

    auto body = parseBlock();
    if (!body) return nullptr;

    return std::make_unique<FuncDefAST>(name, retType, std::move(params), std::move(body));
}

std::unique_ptr<CompUnitAST> Parser::parseCompUnit() {
    auto unit = std::make_unique<CompUnitAST>();
    while (CurTok.isNot(tok::eof)) {
        std::string type = parseType();
        if (type.empty()) {
            if (CurTok.is(tok::eof)) break;
            getNextToken(); // Skip error/stray token
            continue;
        }

        if (CurTok.isNot(tok::identifier)) {
            std::cerr << "Error: Expected identifier after type" << std::endl;
            getNextToken();
            continue;
        }
        std::string name(CurTok.getText());
        getNextToken();

        if (CurTok.is(tok::l_paren)) {
            getNextToken(); // consume '('

            std::vector<std::unique_ptr<FuncFParamAST>> params;
            if (CurTok.isNot(tok::r_paren)) {
                while (true) {
                    std::string paramType = parseType();
                    if (paramType.empty()) {
                         std::cerr << "Error: Expected type in function parameter" << std::endl;
                         break;
                    }
                    if (CurTok.isNot(tok::identifier)) {
                        std::cerr << "Error: Expected parameter name" << std::endl;
                        break;
                    }
                    std::string paramName(CurTok.getText());
                    getNextToken();

                    std::vector<std::unique_ptr<ExprAST>> dims;
                    while (CurTok.is(tok::l_square)) {
                        getNextToken();
                        if (CurTok.is(tok::r_square)) {
                            dims.push_back(nullptr);
                            getNextToken();
                        } else {
                            auto dim = parseExpr();
                            dims.push_back(std::move(dim));
                            if (!expect(tok::r_square)) break;
                        }
                    }
                    params.push_back(std::make_unique<FuncFParamAST>(paramType, paramName, std::move(dims)));

                    if (CurTok.is(tok::comma)) getNextToken();
                    else break;
                }
            }

            if (!expect(tok::r_paren)) continue;

            auto body = parseBlock();
            if (body) {
                unit->addChild(std::make_unique<FuncDefAST>(name, type, std::move(params), std::move(body)));
            }
        } 
        else {
            std::vector<std::unique_ptr<ExprAST>> dims;
            while (CurTok.is(tok::l_square)) {
                getNextToken(); // consume '['
                auto dim = parseExpr();
                if (!dim) break;
                dims.push_back(std::move(dim));
                if (!expect(tok::r_square)) break;
            }

            std::unique_ptr<InitValAST> init = nullptr;
            if (CurTok.is(tok::equal)) {
                getNextToken();
                init = parseInitVal();
            }

            if (expect(tok::semi)) {
                unit->addChild(std::make_unique<VarDeclAST>(type, name, std::move(dims), std::move(init)));
            }
        }
    }
    return unit;
}
