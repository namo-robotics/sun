// this_expr_ast.cpp — ThisExprAST clone implementation

#include "ast/this_expr_ast.h"

std::unique_ptr<ExprAST> ThisExprAST::clone() const {
  auto copy = std::make_unique<ThisExprAST>();
  cloneBase(*copy);
  return copy;
}
