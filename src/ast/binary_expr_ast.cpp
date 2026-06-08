// binary_expr_ast.cpp — BinaryExprAST clone implementation

#include "ast/binary_expr_ast.h"

std::unique_ptr<ExprAST> BinaryExprAST::clone() const {
  auto copy = std::make_unique<BinaryExprAST>(op, LHS->clone(), RHS->clone());
  cloneBase(*copy);
  return copy;
}
