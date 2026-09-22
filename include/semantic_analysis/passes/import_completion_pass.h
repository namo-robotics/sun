#pragma once

#include <vector>

#include "ast/ast_fwd.h"

/** Resolves declarations and checks the meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;

/** Provides the ordered preparation and registration passes for analysis. */
namespace passes {

/** Complete imported interfaces, enums, and bindings without checking compiled
 * bodies. */
class ImportCompletionPass {
 public:
  /** Borrow the analysis session's analyzer. */
  explicit ImportCompletionPass(SemanticAnalyzer& analyzer)
      : analyzer_(analyzer) {}

  /** Complete imported interfaces, enums, and bindings without checking
   * compiled bodies. Borrows imported scopes without changing AST ownership or
   * loading files.
   */
  void run(const std::vector<sun::ast::MoonScopeAST*>& imports) const;

 private:
  SemanticAnalyzer& analyzer_;
};

}  // namespace passes
}  // namespace sun::semantic_analysis
