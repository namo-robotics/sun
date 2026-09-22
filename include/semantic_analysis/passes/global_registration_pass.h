#pragma once

#include "ast/ast_fwd.h"

/** Resolves declarations and checks the meaning of Sun programs. */
namespace sun::semantic_analysis {
class DeclarationTable;

/** Provides the ordered passes for semantic analysis. */
namespace passes {

/** Register named source globals before resolving their uses. */
class GlobalRegistrationPass {
 public:
  /** Borrow the analysis session's declaration table. */
  explicit GlobalRegistrationPass(DeclarationTable& declarations)
      : declarations_(declarations) {}

  /** Register named source globals before resolving their uses. */
  void run(const sun::ast::BlockExprAST& block) const;

 private:
  DeclarationTable& declarations_;
};

}  // namespace passes
}  // namespace sun::semantic_analysis
