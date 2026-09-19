#pragma once

#include "ast/ast_fwd.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Insert field defaults into constructors, creating a constructor when needed.
 */
class FieldInitializerPreparationPass {
 public:
  /** Prepare constructors and field defaults throughout the AST, including bodies. */
  void run(sun::ast::ExprAST& root) const;
};

}  // namespace sun::semantic_analysis
