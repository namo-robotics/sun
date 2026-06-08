// lambda_ast.cpp — LambdaAST clone implementation

#include "ast/lambda_ast.h"

std::unique_ptr<ExprAST> LambdaAST::clone() const {
  auto protoClone = Proto->clone();
  std::unique_ptr<BlockExprAST> bodyClone;
  if (Body) {
    auto bodyExpr = Body->clone();
    bodyClone.reset(static_cast<BlockExprAST*>(bodyExpr.release()));
  }
  auto copy =
      std::make_unique<LambdaAST>(std::move(protoClone), std::move(bodyClone));
  cloneBase(*copy);
  return copy;
}
