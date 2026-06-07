// block_expr_ast.cpp — BlockExprAST clone implementation

#include "ast/block_expr_ast.h"

#include "ast/ast_utils.h"

std::unique_ptr<ExprAST> BlockExprAST::clone() const {
  auto copy = std::make_unique<BlockExprAST>(cloneExprVector(Body));
  cloneBase(*copy);
  return copy;
}
