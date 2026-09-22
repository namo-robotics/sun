// match_expr_ast.h — MatchExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * A payload binding position in a destructuring pattern: Shape.Circle(r)
 */
struct PatternBinding {
  std::string name;         // empty when isWildcard
  bool isWildcard = false;  // '_' in this position
  sun::support::Position location;
  // Set by semantic analysis:
  sun::types::TypePtr resolvedType;  // payload element type
  mutable sun::semantic_analysis::DeclarationIdentity declaration{};
};

/**
 * A single arm in a match expression: pattern => body
 */
struct MatchArm {
  std::unique_ptr<ExprAST> pattern;  // nullptr for wildcard _
  bool isWildcard;                   // true if this arm is _
  bool hasPayloadParens = false;     // pattern had a '(...)' binding list
  std::vector<PatternBinding> bindings;
  std::unique_ptr<ExprAST> body;
  // Set by semantic analysis when the pattern names an enum variant.
  // Valid tags may be negative; the pattern type identifies enum arms.
  int64_t resolvedVariantTag = -1;

  /** Creates a match branch owning its pattern and body. */
  MatchArm(std::unique_ptr<ExprAST> pattern, bool isWildcard,
           std::unique_ptr<ExprAST> body)
      : pattern(std::move(pattern)),
        isWildcard(isWildcard),
        body(std::move(body)) {}

  /**
   * Move constructor
   */
  MatchArm(MatchArm&& other) = default;
  /** Transfers the stored state from another instance during move assignment.
   */
  MatchArm& operator=(MatchArm&& other) = default;

  /**
   * No copy
   */
  MatchArm(const MatchArm&) = delete;
  /** Disallows assignment so ownership and object identity cannot be
   * duplicated. */
  MatchArm& operator=(const MatchArm&) = delete;
};

/** A match expression containing the value to inspect and its pattern arms. */
class MatchExprAST : public ExprAST {
  std::unique_ptr<ExprAST> discriminant;  // The value being matched
  std::vector<MatchArm> arms;             // Match arms

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  MatchExprAST(std::unique_ptr<ExprAST> discriminant,
               std::vector<MatchArm> arms)
      : discriminant(std::move(discriminant)), arms(std::move(arms)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::MATCH; }

  /** Returns a readable representation for diagnostics and debugging. */
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

  /** Returns the enum tag. */
  const ExprAST* getDiscriminant() const { return discriminant.get(); }
  /** Returns the pattern-match branches. */
  const std::vector<MatchArm>& getArms() const { return arms; }
  /** Returns the modifiable pattern-match branches. */
  std::vector<MatchArm>& getArmsMutable() { return arms; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(discriminant);
    for (auto& arm : arms) {
      fn(arm.pattern);
      fn(arm.body);
    }
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Match"; }
};

}  // namespace sun::ast
