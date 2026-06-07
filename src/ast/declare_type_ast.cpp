// declare_type_ast.cpp — DeclareTypeAST clone implementation

#include "ast/declare_type_ast.h"

std::unique_ptr<ExprAST> DeclareTypeAST::clone() const {
  auto copy = std::make_unique<DeclareTypeAST>(typeAnnotation, aliasName);
  cloneBase(*copy);
  if (hasResolvedDeclaredType()) {
    copy->setResolvedDeclaredType(getResolvedDeclaredType());
  }
  return copy;
}
