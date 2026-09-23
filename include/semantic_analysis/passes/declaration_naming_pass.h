#pragma once

#include <string>
#include <vector>

#include "ast/ast_fwd.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

/** Name the current local declaration and its methods without walking bodies.
 */
void assignLocalDeclarationName(sun::ast::ExprAST& declaration,
                                const std::vector<std::string>& scopePath);

/** Assign declaration names without resolving types or registering symbols. */
class DeclarationNamingPass {
 public:
  /** Run this stage across all borrowed imported bundles before the next stage.
   */
  void run(const std::vector<sun::ast::MoonScopeAST*>& imports,
           const std::vector<std::string>& scopePath = {}) const;

  /**
   * Preserve imported and specialized names. Function bodies are named later,
   * after their enclosing signatures resolve; local variables stay local.
   * Optionally skip imported moons, always visiting the bundle being built.
   */
  void run(sun::ast::ExprAST& root,
           const std::vector<std::string>& scopePath = {},
           bool moduleLevel = true, bool skipImportedMoons = false) const;
};

}  // namespace sun::semantic_analysis::passes
