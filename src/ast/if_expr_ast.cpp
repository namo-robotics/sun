// if_expr_ast.cpp — IfExprAST clone implementation

#include "ast/if_expr_ast.h"

std::unique_ptr<ExprAST> IfExprAST::clone() const {
  auto copy = std::make_unique<IfExprAST>(Cond->clone(), Then->clone(),
                                          Else ? Else->clone() : nullptr);
  cloneBase(*copy);
  return copy;
}
