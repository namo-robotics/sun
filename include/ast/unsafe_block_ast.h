// unsafe_block_ast.h — UnsafeBlockAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

namespace sun::ast {

/** Allow unsafe operations in a block or a single expression. */
class UnsafeBlockAST : public ExprAST {
  std::unique_ptr<BlockExprAST> body;
  bool expressionForm;

 public:
  explicit UnsafeBlockAST(std::unique_ptr<BlockExprAST> b,
                          bool expressionForm = false)
      : body(std::move(b)), expressionForm(expressionForm) {}

  /** Whether the source uses a single expression without braces. */
  bool isExpressionForm() const { return expressionForm; }

  ASTNodeType getType() const override { return ASTNodeType::UNSAFE_BLOCK; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (body) body->forEachChildSlot(fn);
  }
  std::string toString() const override {
    return expressionForm ? "unsafe ..." : "unsafe { ... }";
  }
  std::string dotLabel() const override { return "Unsafe"; }

  const BlockExprAST& getBody() const { return *body; }
  BlockExprAST& getBody() { return *body; }
};

}  // namespace sun::ast
