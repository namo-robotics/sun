// bool_literal_ast.cpp — BoolLiteralAST clone implementation

#include "ast/bool_literal_ast.h"

std::unique_ptr<ExprAST> BoolLiteralAST::clone() const {
  auto copy = std::make_unique<BoolLiteralAST>(Value);
  cloneBase(*copy);
  return copy;
}
