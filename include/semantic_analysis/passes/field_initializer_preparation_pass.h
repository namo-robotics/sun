#pragma once

#include <vector>

#include "ast/ast_fwd.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

/** Insert field defaults into constructors, creating a constructor when needed.
 */
class FieldInitializerPreparationPass {
 public:
  /** Run this stage across all borrowed imported bundles before the next stage. */
  void run(const std::vector<sun::ast::MoonScopeAST*>& imports) const;

  /** Prepare constructors and field defaults throughout the AST, including
   * bodies. Optionally skip imported moon subtrees; the bundle being built
   * always remains source and is visited.
   */
  void run(sun::ast::ExprAST& root, bool skipImportedMoons = false) const;
};

}  // namespace sun::semantic_analysis::passes
