// indexed_assignment_ast.cpp — IndexedAssignmentAST clone implementation

#include "ast/indexed_assignment_ast.h"

std::unique_ptr<ExprAST> IndexedAssignmentAST::clone() const {
  auto copy =
      std::make_unique<IndexedAssignmentAST>(target->clone(), value->clone());
  cloneBase(*copy);
  return copy;
}
