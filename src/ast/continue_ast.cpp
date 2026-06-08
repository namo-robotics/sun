// continue_ast.cpp — ContinueAST clone implementation

#include "ast/continue_ast.h"

std::unique_ptr<ExprAST> ContinueAST::clone() const {
  auto copy = std::make_unique<ContinueAST>();
  cloneBase(*copy);
  return copy;
}
