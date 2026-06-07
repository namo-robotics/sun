// generic_call_ast.cpp — GenericCallAST clone implementation

#include "ast/generic_call_ast.h"

std::unique_ptr<ExprAST> GenericCallAST::clone() const {
  auto copy = std::make_unique<GenericCallAST>(
      functionName, cloneTypeAnnotationVector(typeArguments),
      cloneExprVector(args));
  cloneBase(*copy);
  if (hasGenericFunctionAST()) {
    copy->setGenericFunctionAST(getGenericFunctionAST());
  }
  if (hasResolvedTypeArgs()) {
    copy->setResolvedTypeArgs(getResolvedTypeArgs());
  }
  return copy;
}
