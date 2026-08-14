// match_expr_ast.h — MatchExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

// A payload binding position in a destructuring pattern: Shape.Circle(r)
struct PatternBinding {
  std::string name;         // empty when isWildcard
  bool isWildcard = false;  // '_' in this position
  Position location;
  // Set by semantic analysis:
  sun::TypePtr resolvedType;        // payload element type
  std::string resolvedMangledName;  // scoped name for codegen
};

// A single arm in a match expression: pattern => body
struct MatchArm {
  std::unique_ptr<ExprAST> pattern;  // nullptr for wildcard _
  bool isWildcard;                   // true if this arm is _
  bool hasPayloadParens = false;     // pattern had a '(...)' binding list
  std::vector<PatternBinding> bindings;
  std::unique_ptr<ExprAST> body;
  // Set by semantic analysis: >=0 when the pattern names a variant of the
  // discriminant enum
  int resolvedVariantTag = -1;

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
  std::vector<MatchArm>& getArmsMutable() { return arms; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(discriminant);
    for (auto& arm : arms) {
      fn(arm.pattern);
      fn(arm.body);
    }
  }
  std::string dotLabel() const override { return "Match"; }
};
