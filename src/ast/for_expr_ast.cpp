// for_expr_ast.cpp — ForExprAST clone implementation

#include "ast/for_expr_ast.h"

std::unique_ptr<ExprAST> ForExprAST::clone() const {
  auto copy = std::make_unique<ForExprAST>(
      Init ? Init->clone() : nullptr, Condition ? Condition->clone() : nullptr,
      Increment ? Increment->clone() : nullptr, Body->clone());
  cloneBase(*copy);
  return copy;
}
