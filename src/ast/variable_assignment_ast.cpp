// variable_assignment_ast.cpp — VariableAssignmentAST clone implementation

#include "ast/variable_assignment_ast.h"

std::unique_ptr<ExprAST> VariableAssignmentAST::clone() const {
  auto copy = std::make_unique<VariableAssignmentAST>(name, value->clone());
  cloneBase(*copy);
  return copy;
}
