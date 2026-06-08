// variable_reference_ast.cpp — VariableReferenceAST clone implementation

#include "ast/variable_reference_ast.h"

std::unique_ptr<ExprAST> VariableReferenceAST::clone() const {
  auto copy = std::make_unique<VariableReferenceAST>(Name);
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}
