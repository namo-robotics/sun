// paren_expr_ast.h — Parenthesized expression (lossless parse tree only)

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Grouping parentheses: (expr). Preserved by the parser for a lossless parse
 * tree; the lowering pass unwraps it before semantic analysis, so it never
 * reaches the borrow checker or codegen.
 */
class ParenExprAST : public ExprAST {
  std::unique_ptr<ExprAST> inner_;

 public:
  /** Creates this syntax node from its operands and declaration information. */
  explicit ParenExprAST(std::unique_ptr<ExprAST> inner)
      : inner_(std::move(inner)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::PAREN_EXPR; }

  /** Returns the inner stored by this object. */
  const ExprAST* getInner() const { return inner_.get(); }

  /**
   * Used by the lowering pass to unwrap the node
   */
  std::unique_ptr<ExprAST> takeInner() { return std::move(inner_); }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override { fn(inner_); }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return "(" + (inner_ ? inner_->toString() : "") + ")";
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "ParenExpr"; }
};

}  // namespace sun::ast
