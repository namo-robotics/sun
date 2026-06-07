// bool_literal_ast.h — BoolLiteralAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

class BoolLiteralAST : public ExprAST {
  bool Value;

 public:
  explicit BoolLiteralAST(bool Value) : Value(Value) {}
  ASTNodeType getType() const override { return ASTNodeType::BOOL_LITERAL; }
  std::string toString() const override { return Value ? "true" : "false"; }
  std::string dotLabel() const override { return "Bool\n" + toString(); }
  std::unique_ptr<ExprAST> clone() const override;
  bool getValue() const { return Value; }
};
