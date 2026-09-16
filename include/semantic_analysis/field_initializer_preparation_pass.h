#pragma once

#include "ast/ast_fwd.h"

namespace sun {

/** Insert field defaults into constructors, creating a constructor when needed.
 */
class FieldInitializerPreparationPass {
 public:
  /** Prepare constructors and field defaults throughout the AST, including bodies. */
  void run(ExprAST& root) const;
};

}  // namespace sun
