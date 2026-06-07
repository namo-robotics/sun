// variable_creation_ast.cpp — VariableCreationAST clone implementation

#include "ast/variable_creation_ast.h"

std::unique_ptr<ExprAST> VariableCreationAST::clone() const {
  std::optional<TypeAnnotation> typeClone;
  if (typeAnnotation) {
    typeClone = *typeAnnotation;  // TypeAnnotation has copy constructor
  }
  auto copy = std::make_unique<VariableCreationAST>(name, value->clone(),
                                                    std::move(typeClone));
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}
