// unsafe_block_ast.h — UnsafeBlockAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** Allow unsafe operations in a block or a single expression. */
class UnsafeBlockAST : public ExprAST {
  std::unique_ptr<BlockExprAST> body;
  bool expressionForm;

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  explicit UnsafeBlockAST(std::unique_ptr<BlockExprAST> b,
                          bool expressionForm = false)
      : body(std::move(b)), expressionForm(expressionForm) {}

  /** Whether the source uses a single expression without braces. */
  bool isExpressionForm() const { return expressionForm; }

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::UNSAFE_BLOCK; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (body) body->forEachChildSlot(fn);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return expressionForm ? "unsafe ..." : "unsafe { ... }";
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Unsafe"; }

  /** Provides access to the expressions that make up the body. */
  const BlockExprAST& getBody() const { return *body; }
  /** Provides access to the expressions that make up the body. */
  BlockExprAST& getBody() { return *body; }
};

}  // namespace sun::ast
