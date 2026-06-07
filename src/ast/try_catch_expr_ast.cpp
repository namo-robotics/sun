// try_catch_expr_ast.cpp — TryCatchExprAST clone implementation

#include "ast/try_catch_expr_ast.h"

std::unique_ptr<ExprAST> TryCatchExprAST::clone() const {
  auto tryClone = tryBlock->clone();
  std::unique_ptr<BlockExprAST> tryBlockClone(
      static_cast<BlockExprAST*>(tryClone.release()));

  CatchClause catchClone;
  catchClone.bindingName = catchClause.bindingName;
  if (catchClause.bindingType) {
    catchClone.bindingType = *catchClause.bindingType;
  }
  auto bodyClone = catchClause.body->clone();
  catchClone.body.reset(static_cast<BlockExprAST*>(bodyClone.release()));

  auto copy = std::make_unique<TryCatchExprAST>(std::move(tryBlockClone),
                                                std::move(catchClone));
  cloneBase(*copy);
  return copy;
}
