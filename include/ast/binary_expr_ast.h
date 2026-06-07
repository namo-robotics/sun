// binary_expr_ast.h — BinaryExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"
#include "lexer.h"

class BinaryExprAST : public ExprAST {
  Token op;
  std::unique_ptr<ExprAST> LHS, RHS;

 public:
  BinaryExprAST(Token Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Op.start), op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  ASTNodeType getType() const override { return ASTNodeType::BINARY; }
  std::string toString() const override {
    return "(" + LHS->toString() + " " + op.text + " " + RHS->toString() + ")";
  }
  Token getOp() const { return op; }
  const ExprAST* getLHS() const { return LHS.get(); }
  const ExprAST* getRHS() const { return RHS.get(); }
  std::string dotLabel() const override { return "Binary\n" + op.text; }
  std::unique_ptr<ExprAST> clone() const override;
};
