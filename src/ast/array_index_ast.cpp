// array_index_ast.cpp — ArrayIndexAST clone implementation

#include "ast/array_index_ast.h"

#include "ast/ast_utils.h"

std::unique_ptr<ExprAST> ArrayIndexAST::clone() const {
  auto copy =
      std::make_unique<ArrayIndexAST>(array->clone(), cloneExprVector(indices));
  cloneBase(*copy);
  return copy;
}
