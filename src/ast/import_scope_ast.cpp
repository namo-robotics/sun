// import_scope_ast.cpp — ImportScopeAST clone implementation

#include "ast/import_scope_ast.h"

std::unique_ptr<ExprAST> ImportScopeAST::clone() const {
  auto bodyClone = std::unique_ptr<BlockExprAST>(
      static_cast<BlockExprAST*>(body->clone().release()));
  auto copy = std::make_unique<ImportScopeAST>(sourceFile, std::move(bodyClone),
                                               contentHash);
  cloneBase(*copy);
  return copy;
}
