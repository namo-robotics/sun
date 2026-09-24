#pragma once

#include <vector>

#include "ast/ast_fwd.h"

/** Resolves declarations and checks the meaning of Sun programs. */
namespace sun::semantic_analysis {
class DeclarationTable;

/** Provides the ordered preparation and registration passes for analysis. */
namespace passes {

/** Register every imported declaration record before dependency validation. */
class ImportLibraryDeclarationsPass {
 public:
  /** Borrow the analysis session's declarations. */
  explicit ImportLibraryDeclarationsPass(DeclarationTable& declarations)
      : declarations_(declarations) {}

  /** Register every imported declaration record before dependency validation.
   * Borrows imported scopes without changing AST ownership or loading files.
   */
  void run(const std::vector<sun::ast::MoonScopeAST*>& imports) const;

 private:
  DeclarationTable& declarations_;
};

}  // namespace passes
}  // namespace sun::semantic_analysis
