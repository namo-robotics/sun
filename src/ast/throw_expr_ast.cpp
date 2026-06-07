// throw_expr_ast.cpp — ThrowExprAST clone implementation

#include "ast/throw_expr_ast.h"

std::unique_ptr<ExprAST> ThrowExprAST::clone() const {
  auto copy = std::make_unique<ThrowExprAST>(errorExpr->clone());
  cloneBase(*copy);
  return copy;
}
