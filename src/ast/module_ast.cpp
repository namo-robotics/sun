// module_ast.cpp — ModuleAST clone implementation

#include "ast/module_ast.h"

std::unique_ptr<ExprAST> ModuleAST::clone() const {
  auto bodyClone = body->clone();
  std::unique_ptr<BlockExprAST> bodyBlockClone(
      static_cast<BlockExprAST*>(bodyClone.release()));
  auto copy = std::make_unique<ModuleAST>(name, std::move(bodyBlockClone));
  cloneBase(*copy);
  return copy;
}
