// return_expr_ast.cpp — ReturnExprAST clone implementation

#include "ast/return_expr_ast.h"

std::unique_ptr<ExprAST> ReturnExprAST::clone() const {
  auto copy = std::make_unique<ReturnExprAST>(Value ? Value->clone() : nullptr);
  cloneBase(*copy);
  return copy;
}
