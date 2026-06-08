// number_expr_ast.cpp — NumberExprAST clone implementation

#include "ast/number_expr_ast.h"

std::unique_ptr<ExprAST> NumberExprAST::clone() const {
  auto copy = isInteger() ? std::make_unique<NumberExprAST>(getIntVal())
                          : std::make_unique<NumberExprAST>(getFloatVal());
  cloneBase(*copy);
  return copy;
}
