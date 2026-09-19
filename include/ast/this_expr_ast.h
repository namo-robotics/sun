// this_expr_ast.h — ThisExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * this keyword - reference to current object instance
 */
class ThisExprAST : public ExprAST {
 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  ThisExprAST() = default;

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::THIS; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "this"; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "this"; }
};

}  // namespace sun::ast
