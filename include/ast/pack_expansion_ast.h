// pack_expansion_ast.h — PackExpansionAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Pack expansion expression: args...
 * Expands a variadic parameter pack in a call expression
 */
class PackExpansionAST : public ExprAST {
  std::string packName;  // Name of the variadic parameter to expand

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  explicit PackExpansionAST(std::string name) : packName(std::move(name)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::PACK_EXPANSION; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return packName + "..."; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "PackExpand\n..."; }
  /** Returns the pack name stored by this object. */
  const std::string& getPackName() const { return packName; }
};

}  // namespace sun::ast
