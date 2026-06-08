// match_expr_ast.cpp — MatchExprAST clone implementation

#include "ast/match_expr_ast.h"

std::unique_ptr<ExprAST> MatchExprAST::clone() const {
  std::vector<MatchArm> armsClone;
  armsClone.reserve(arms.size());
  for (const auto& arm : arms) {
    armsClone.emplace_back(arm.pattern ? arm.pattern->clone() : nullptr,
                           arm.isWildcard, arm.body->clone());
  }
  auto copy = std::make_unique<MatchExprAST>(discriminant->clone(),
                                             std::move(armsClone));
  cloneBase(*copy);
  return copy;
}
