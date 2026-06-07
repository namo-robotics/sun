// unary_expr_ast.cpp — UnaryExprAST clone implementation

#include "ast/unary_expr_ast.h"

std::unique_ptr<ExprAST> UnaryExprAST::clone() const {
  auto copy = std::make_unique<UnaryExprAST>(op, Operand->clone());
  cloneBase(*copy);
  return copy;
}
