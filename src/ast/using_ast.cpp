// using_ast.cpp — UsingAST clone implementation

#include "ast/using_ast.h"

std::unique_ptr<ExprAST> UsingAST::clone() const {
  auto copy = std::make_unique<UsingAST>(namespacePath, target);
  cloneBase(*copy);
  return copy;
}
