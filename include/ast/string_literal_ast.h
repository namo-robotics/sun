// string_literal_ast.h — StringLiteralAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

class StringLiteralAST : public ExprAST {
  std::string Value;

 public:
  explicit StringLiteralAST(std::string Value) : Value(std::move(Value)) {}
  ASTNodeType getType() const override { return ASTNodeType::STRING_LITERAL; }
  std::string toString() const override { return "\"" + Value + "\""; }
  std::string dotLabel() const override { return "String\n" + toString(); }
  const std::string& getValue() const { return Value; }
};
