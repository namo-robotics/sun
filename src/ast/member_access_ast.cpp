// member_access_ast.cpp — MemberAccessAST clone implementation

#include "ast/member_access_ast.h"

std::unique_ptr<ExprAST> MemberAccessAST::clone() const {
  auto copy = std::make_unique<MemberAccessAST>(
      object->clone(), memberName, cloneTypeAnnotationVector(typeArguments));
  cloneBase(*copy);
  if (hasResolvedTypeArgs()) {
    copy->setResolvedTypeArgs(getResolvedTypeArgs());
  }
  if (hasResolvedQualifiedName()) {
    copy->setResolvedQualifiedName(getResolvedQualifiedName());
  }
  return copy;
}
