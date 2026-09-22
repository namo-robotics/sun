#pragma once

#include <vector>

#include "ast/ast_fwd.h"

/** Resolves declarations and checks the meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticContext;

/** Provides the ordered preparation and registration passes for analysis. */
namespace passes {

/** Validate exact import requirements after every bundle record is registered.
 */
class ImportDependencyValidationPass {
 public:
  /** Borrow the analysis session's context. */
  explicit ImportDependencyValidationPass(SemanticContext& context)
      : context_(context) {}

  /** Validate exact import requirements after every bundle record is
   * registered. Borrows imported scopes without changing AST ownership or
   * loading files.
   */
  void run(const std::vector<sun::ast::MoonScopeAST*>& imports) const;

 private:
  SemanticContext& context_;
};

}  // namespace passes
}  // namespace sun::semantic_analysis
