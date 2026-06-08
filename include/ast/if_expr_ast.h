// if_expr_ast.h — IfExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

 public:
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
      : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}
  ASTNodeType getType() const override { return ASTNodeType::IF; }
  std::string toString() const override {
    std::string result = "if (" + Cond->toString() + ") " + Then->toString();
    if (Else) result += " else " + Else->toString();
    return result;
  }
  ExprAST* getCond() const { return Cond.get(); }
  ExprAST* getThen() const { return Then.get(); }
  ExprAST* getElse() const { return Else.get(); }
  std::string dotLabel() const override { return "If"; }
};
