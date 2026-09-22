/** Declares AST-only preparation of imported bundle declarations. */
#pragma once

#include "ast/block_expr_ast.h"
#include "semantic_analysis/semantic_context.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

/** Registers imported identities and validates exact dependencies before any
 * syntax identities or types are resolved. The complete AST must already
 * contain its imported bundles as top-level MoonScopeAST nodes. No files are
 * read, and imported signatures and bodies are left for later passes.
 */
class MoonImportPreparationPass {
 public:
  /** Borrows the analysis session that owns the declaration table. */
  explicit MoonImportPreparationPass(SemanticContext& ctx) : ctx_(ctx) {}

  /** Registers all imported records before checking any bundle requirements,
   * without changing the tree or preparing the bundle being built.
   */
  void run(const sun::ast::BlockExprAST& block) const;

 private:
  SemanticContext& ctx_;
};

}  // namespace sun::semantic_analysis::passes
