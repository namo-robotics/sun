/** Declares the pass that makes types and templates available before
 * resolution. */
#pragma once

#include <vector>

#include "semantic_analysis/semantic_context.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

/**
 * Register module scopes, nominal type names, and generic templates throughout
 * the module tree. Requires declaration identities and qualified names, but
 * does not resolve field or signature annotations, specialize templates, or
 * analyze bodies. Local declarations remain the responsibility of body
 * analysis.
 */
class TypeRegistrationPass {
 public:
  /** Run this stage across all borrowed imported bundles before the next stage.
   */
  void run(const std::vector<sun::ast::MoonScopeAST*>& imports);

  /** Borrow the shared context that owns scopes and registered types. */
  explicit TypeRegistrationPass(SemanticContext& ctx) : ctx_(ctx) {}

  /** Register source declarations, skipping imported bundle wrappers whose
   * contents are registered separately by import preparation.
   */
  void run(sun::ast::BlockExprAST& block);

 private:
  SemanticContext& ctx_;
};

}  // namespace sun::semantic_analysis::passes
