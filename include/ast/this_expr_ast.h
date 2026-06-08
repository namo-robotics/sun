// this_expr_ast.h — ThisExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// this keyword - reference to current object instance
class ThisExprAST : public ExprAST {
 public:
  ThisExprAST() = default;

  ASTNodeType getType() const override { return ASTNodeType::THIS; }
  std::string toString() const override { return "this"; }
  std::string dotLabel() const override { return "this"; }
  std::unique_ptr<ExprAST> clone() const override;
};
