// break_ast.cpp — BreakAST clone implementation

#include "ast/break_ast.h"

std::unique_ptr<ExprAST> BreakAST::clone() const {
  auto copy = std::make_unique<BreakAST>();
  cloneBase(*copy);
  return copy;
}
