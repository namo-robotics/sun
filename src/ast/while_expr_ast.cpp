// while_expr_ast.cpp — WhileExprAST clone implementation

#include "ast/while_expr_ast.h"

std::unique_ptr<ExprAST> WhileExprAST::clone() const {
  auto copy = std::make_unique<WhileExprAST>(Condition->clone(), Body->clone());
  cloneBase(*copy);
  return copy;
}
