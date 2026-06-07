// pack_expansion_ast.h — PackExpansionAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Pack expansion expression: args...
// Expands a variadic parameter pack in a call expression
class PackExpansionAST : public ExprAST {
  std::string packName;  // Name of the variadic parameter to expand

 public:
  explicit PackExpansionAST(std::string name) : packName(std::move(name)) {}
  ASTNodeType getType() const override { return ASTNodeType::PACK_EXPANSION; }
  std::string toString() const override { return packName + "..."; }
  std::string dotLabel() const override { return "PackExpand\n..."; }
  std::unique_ptr<ExprAST> clone() const override;
  const std::string& getPackName() const { return packName; }
};
