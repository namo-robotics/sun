// match_expr_ast.h — MatchExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

// A single arm in a match expression: pattern => body
struct MatchArm {
  std::unique_ptr<ExprAST> pattern;  // nullptr for wildcard _
  bool isWildcard;                   // true if this arm is _
  std::unique_ptr<ExprAST> body;

  MatchArm(std::unique_ptr<ExprAST> pattern, bool isWildcard,
           std::unique_ptr<ExprAST> body)
      : pattern(std::move(pattern)),
        isWildcard(isWildcard),
        body(std::move(body)) {}

  // Move constructor
  MatchArm(MatchArm&& other) = default;
  MatchArm& operator=(MatchArm&& other) = default;

  // No copy
  MatchArm(const MatchArm&) = delete;
  MatchArm& operator=(const MatchArm&) = delete;
};

class MatchExprAST : public ExprAST {
  std::unique_ptr<ExprAST> discriminant;  // The value being matched
  std::vector<MatchArm> arms;             // Match arms

 public:
  MatchExprAST(std::unique_ptr<ExprAST> discriminant,
               std::vector<MatchArm> arms)
      : discriminant(std::move(discriminant)), arms(std::move(arms)) {}

  ASTNodeType getType() const override { return ASTNodeType::MATCH; }

  std::string toString() const override {
    std::string result = "match " + discriminant->toString() + " {";
    for (size_t i = 0; i < arms.size(); ++i) {
      if (i > 0) result += ", ";
      if (arms[i].isWildcard) {
        result += "_";
      } else {
        result += arms[i].pattern->toString();
      }
      result += " => " + arms[i].body->toString();
    }
    result += "}";
    return result;
  }

  const ExprAST* getDiscriminant() const { return discriminant.get(); }
  const std::vector<MatchArm>& getArms() const { return arms; }
  std::string dotLabel() const override { return "Match"; }
};
