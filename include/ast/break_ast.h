// break_ast.h — BreakAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Break statement: break;
 * Exits the innermost enclosing loop
 */
class BreakAST : public ExprAST {
 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  BreakAST() = default;

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::BREAK_STMT; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "break"; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Break"; }
};

}  // namespace sun::ast
