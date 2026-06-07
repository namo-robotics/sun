// reference_creation_ast.cpp — ReferenceCreationAST clone implementation

#include "ast/reference_creation_ast.h"

std::unique_ptr<ExprAST> ReferenceCreationAST::clone() const {
  auto copy =
      std::make_unique<ReferenceCreationAST>(name, target->clone(), mutable_);
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}
