// function_ast.cpp — FunctionAST clone implementation

#include "ast/function_ast.h"

std::unique_ptr<ExprAST> FunctionAST::clone() const {
  auto protoClone = Proto->clone();
  std::unique_ptr<BlockExprAST> bodyClone;
  if (Body) {
    auto bodyExpr = Body->clone();
    bodyClone.reset(static_cast<BlockExprAST*>(bodyExpr.release()));
  }
  auto copy = std::make_unique<FunctionAST>(std::move(protoClone),
                                            std::move(bodyClone));
  cloneBase(*copy);
  copy->setSourceText(sourceText_);
  return copy;
}
