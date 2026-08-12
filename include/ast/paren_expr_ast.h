// paren_expr_ast.h — Parenthesized expression (lossless parse tree only)

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Grouping parentheses: (expr). Preserved by the parser for a lossless parse
// tree; the lowering pass unwraps it before semantic analysis, so it never
// reaches the borrow checker or codegen.
class ParenExprAST : public ExprAST {
  std::unique_ptr<ExprAST> inner_;

 public:
  explicit ParenExprAST(std::unique_ptr<ExprAST> inner)
      : inner_(std::move(inner)) {}

  ASTNodeType getType() const override { return ASTNodeType::PAREN_EXPR; }

  const ExprAST* getInner() const { return inner_.get(); }

  // Used by the lowering pass to unwrap the node
  std::unique_ptr<ExprAST> takeInner() { return std::move(inner_); }

  void forEachChildSlot(const ChildSlotFn& fn) override { fn(inner_); }

  std::string toString() const override {
    return "(" + (inner_ ? inner_->toString() : "") + ")";
  }
  std::string dotLabel() const override { return "ParenExpr"; }
};
