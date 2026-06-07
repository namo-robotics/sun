// null_literal_ast.cpp — NullLiteralAST clone implementation

#include "ast/null_literal_ast.h"

std::unique_ptr<ExprAST> NullLiteralAST::clone() const {
  auto copy = std::make_unique<NullLiteralAST>();
  cloneBase(*copy);
  return copy;
}
