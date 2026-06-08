// slice_expr_ast.cpp — SliceExprAST clone implementation

#include "ast/slice_expr_ast.h"

std::unique_ptr<ExprAST> SliceExprAST::clone() const {
  std::unique_ptr<ExprAST> startClone = start_ ? start_->clone() : nullptr;
  std::unique_ptr<ExprAST> endClone = end_ ? end_->clone() : nullptr;
  auto copy = isRange_ ? std::make_unique<SliceExprAST>(
                             std::move(startClone), std::move(endClone), true)
                       : std::make_unique<SliceExprAST>(std::move(startClone));
  cloneBase(*copy);
  return copy;
}
