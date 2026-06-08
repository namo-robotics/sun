// null_literal_ast.h — NullLiteralAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

class NullLiteralAST : public ExprAST {
 public:
  NullLiteralAST() = default;
  ASTNodeType getType() const override { return ASTNodeType::NULL_LITERAL; }
  std::string toString() const override { return "null"; }
  std::string dotLabel() const override { return "null"; }
};
