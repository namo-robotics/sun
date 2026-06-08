// import_ast.cpp — ImportAST clone implementation

#include "ast/import_ast.h"

std::unique_ptr<ExprAST> ImportAST::clone() const {
  auto copy = std::make_unique<ImportAST>(path);
  cloneBase(*copy);
  return copy;
}
