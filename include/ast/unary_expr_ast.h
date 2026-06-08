// unary_expr_ast.h — UnaryExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"
#include "lexer.h"

class UnaryExprAST : public ExprAST {
  Token op;
  std::unique_ptr<ExprAST> Operand;

 public:
  UnaryExprAST(Token op, std::unique_ptr<ExprAST> operand)
      : ExprAST(op.start), op(op), Operand(std::move(operand)) {}
  ASTNodeType getType() const override { return ASTNodeType::UNARY; }
  std::string toString() const override {
    return op.text + Operand->toString();
  }
  Token getOp() const { return op; }
  const ExprAST* getOperand() const { return Operand.get(); }
  std::string dotLabel() const override { return "Unary\n" + op.text; }
  std::unique_ptr<ExprAST> clone() const override;
};
