#pragma once

#include <string>
#include <vector>

#include "ast/ast_fwd.h"

namespace sun {

/** Name the current local declaration and its methods without walking bodies.
 */
void assignLocalDeclarationName(ExprAST& declaration,
                                const std::vector<std::string>& scopePath,
                                const std::vector<std::string>& modulePath);

/** Assign declaration names without resolving types or registering symbols. */
class DeclarationNamingPass {
 public:
  /**
   * Preserve imported and specialized names. Function bodies are named later,
   * after their enclosing signatures resolve; local variables stay local.
   */
  void run(ExprAST& root, const std::vector<std::string>& scopePath = {},
           const std::vector<std::string>& modulePath = {},
           bool moduleLevel = true) const;

};

}  // namespace sun
