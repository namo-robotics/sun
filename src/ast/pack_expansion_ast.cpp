// pack_expansion_ast.cpp — PackExpansionAST clone implementation

#include "ast/pack_expansion_ast.h"

std::unique_ptr<ExprAST> PackExpansionAST::clone() const {
  auto copy = std::make_unique<PackExpansionAST>(packName);
  cloneBase(*copy);
  return copy;
}
