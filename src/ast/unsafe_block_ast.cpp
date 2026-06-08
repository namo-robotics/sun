// unsafe_block_ast.cpp — UnsafeBlockAST clone implementation

#include "ast/unsafe_block_ast.h"

std::unique_ptr<ExprAST> UnsafeBlockAST::clone() const {
  auto bodyClone = std::unique_ptr<BlockExprAST>(
      static_cast<BlockExprAST*>(body->clone().release()));
  auto copy = std::make_unique<UnsafeBlockAST>(std::move(bodyClone));
  cloneBase(*copy);
  return copy;
}
