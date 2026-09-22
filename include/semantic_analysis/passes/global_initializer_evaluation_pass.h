#pragma once

#include "ast/ast_fwd.h"

/** Resolves declarations and checks the meaning of Sun programs. */
namespace sun::semantic_analysis {
class DeclarationTable;

/** Provides the ordered passes for semantic analysis. */
namespace passes {

/** Evaluate global initializers after all source bodies have been checked. */
class GlobalInitializerEvaluationPass {
 public:
  /** Borrow the analysis session's declaration table. */
  explicit GlobalInitializerEvaluationPass(const DeclarationTable& declarations)
      : declarations_(declarations) {}

  /** Evaluate global initializers after all source bodies have been checked. */
  void run(const sun::ast::BlockExprAST& block) const;

 private:
  const DeclarationTable& declarations_;
};

}  // namespace passes
}  // namespace sun::semantic_analysis
