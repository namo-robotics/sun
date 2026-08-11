// ternary_expr_ast.h — TernaryExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Ternary conditional: cond ? thenExpr : elseExpr. Always value-producing;
// branch types are unified by the semantic analyzer (exact match, integer
// literal coercion, or numeric widening).
class TernaryExprAST : public ExprAST {
  std::unique_ptr<ExprAST> cond, thenExpr, elseExpr;

 public:
  TernaryExprAST(std::unique_ptr<ExprAST> cond,
                 std::unique_ptr<ExprAST> thenExpr,
                 std::unique_ptr<ExprAST> elseExpr, Position loc)
      : ExprAST(loc),
        cond(std::move(cond)),
        thenExpr(std::move(thenExpr)),
        elseExpr(std::move(elseExpr)) {}

  ASTNodeType getType() const override { return ASTNodeType::TERNARY; }
  std::string toString() const override {
    return cond->toString() + " ? " + thenExpr->toString() + " : " +
           elseExpr->toString();
  }
  ExprAST* getCond() const { return cond.get(); }
  ExprAST* getThen() const { return thenExpr.get(); }
  ExprAST* getElse() const { return elseExpr.get(); }
  std::string dotLabel() const override { return "Ternary\n?:"; }
};
