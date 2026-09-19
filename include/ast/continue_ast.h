// continue_ast.h — ContinueAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Continue statement: continue;
 * Jumps to the next iteration of the innermost enclosing loop
 */
class ContinueAST : public ExprAST {
 public:
  /** Creates this syntax node from its operands and declaration information. */
  ContinueAST() = default;

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::CONTINUE_STMT; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "continue"; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Continue"; }
};

}  // namespace sun::ast
