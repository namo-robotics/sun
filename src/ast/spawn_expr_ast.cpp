// spawn_expr_ast.cpp — SpawnExprAST clone implementation

#include "ast/spawn_expr_ast.h"

std::unique_ptr<ExprAST> SpawnExprAST::clone() const {
  auto copy = std::make_unique<SpawnExprAST>(Lambda->clone());
  cloneBase(*copy);
  return copy;
}
