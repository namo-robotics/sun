// string_literal_ast.cpp — StringLiteralAST clone implementation

#include "ast/string_literal_ast.h"

std::unique_ptr<ExprAST> StringLiteralAST::clone() const {
  auto copy = std::make_unique<StringLiteralAST>(Value);
  cloneBase(*copy);
  return copy;
}
