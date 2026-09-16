#pragma once

#include "ast/ast_fwd.h"

class SemanticContext;

namespace sun {

/** Name local declarations after entering their resolved function scope. */
class LocalDeclarationNamingPass {
 public:
  /** Use the enclosing function identity and module from the shared context. */
  explicit LocalDeclarationNamingPass(const SemanticContext& context)
      : context_(context) {}

  /** Name local types and their methods without qualifying local variables. */
  void run(BlockExprAST& body) const;

 private:
  const SemanticContext& context_;
};

}  // namespace sun
