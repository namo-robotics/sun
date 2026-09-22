#pragma once

#include <functional>
#include <vector>

#include "ast/ast_fwd.h"
#include "semantic_analysis/declaration_table.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {
using sun::ast::ExprAST;

/** Assign identities throughout a syntax tree without resolving signatures. */
class DeclarationIdentityPass {
  DeclarationTable& table_;

 public:
  /** Run this stage across all borrowed imported bundles before the next stage. */
  void run(const std::vector<sun::ast::MoonScopeAST*>& imports) const;

  /** Use the declaration table owned by the analysis session. */
  explicit DeclarationIdentityPass(DeclarationTable& table) : table_(table) {}
  /** Register source or generated declarations, retaining assigned identities.
   * Imported declaration records must already be registered by import
   * preparation before attaching their syntax.
   */
  void run(const ExprAST& root, DeclarationId owner = {},
           DeclarationId module = {}, const ExprAST* origin = nullptr) const;

  /** Assign tree identities while optionally skipping imported moon subtrees.
   * The bundle being built is always visited as ordinary source. Invoke the
   * optional callback once all visited declarations have identities.
   */
  void run(const ExprAST& root, bool skipImportedMoons,
           const std::function<void()>& declarationsReady = {}) const;

 private:
  /** Preserve ownership and traversal options while visiting child syntax. */
  void visit(const ExprAST& root, DeclarationId owner, DeclarationId module,
             const ExprAST* origin, bool skipImportedMoons) const;
};

/** Clear computed annotations throughout a tree while preserving identities. */
void clearComputedAnalysis(const ExprAST& root);

/** Discard tree annotations before attaching it to a new analysis session. */
void resetAnalysisSession(const ExprAST& root);

}  // namespace sun::semantic_analysis::passes
