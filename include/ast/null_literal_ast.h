// null_literal_ast.h — NullLiteralAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** The null pointer literal before its target type is resolved. */
class NullLiteralAST : public ExprAST {
 public:
  /** Creates this syntax node from its operands and declaration information. */
  NullLiteralAST() = default;
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::NULL_LITERAL; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "null"; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "null"; }
};

}  // namespace sun::ast
