#pragma once

#include "ast/ast_fwd.h"
#include "semantic_analysis/declaration_table.h"

namespace sun {

/** Assign identities throughout a syntax tree without resolving signatures. */
class DeclarationIdentityPass {
  DeclarationTable& table_;

 public:
  /** Use the declaration table owned by the analysis session. */
  explicit DeclarationIdentityPass(DeclarationTable& table) : table_(table) {}
  /** Register source or generated declarations, retaining assigned identities.
   */
  void run(const ExprAST& root, DeclarationId owner = {},
           DeclarationId module = {}) const;
};

/** Clear computed annotations throughout a tree while preserving identities. */
void clearComputedAnalysis(const ExprAST& root);

/** Discard tree annotations before attaching it to a new analysis session. */
void resetAnalysisSession(const ExprAST& root);

}  // namespace sun
