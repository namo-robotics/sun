// for_in_expr_ast.cpp — ForInExprAST clone implementation

#include "ast/for_in_expr_ast.h"

std::unique_ptr<ExprAST> ForInExprAST::clone() const {
  auto copy = std::make_unique<ForInExprAST>(LoopVar, LoopVarType,
                                             Iterable->clone(), Body->clone());
  cloneBase(*copy);
  if (hasResolvedLoopVarType()) {
    copy->setResolvedLoopVarType(getResolvedLoopVarType());
  }
  return copy;
}
