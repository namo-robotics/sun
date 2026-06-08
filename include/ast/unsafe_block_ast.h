// unsafe_block_ast.h — UnsafeBlockAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

// Unsafe block: unsafe { ... }
// Marks a block where unsafe operations (raw pointer ops, intrinsics) are
// allowed
class UnsafeBlockAST : public ExprAST {
  std::unique_ptr<BlockExprAST> body;

 public:
  explicit UnsafeBlockAST(std::unique_ptr<BlockExprAST> b)
      : body(std::move(b)) {}

  ASTNodeType getType() const override { return ASTNodeType::UNSAFE_BLOCK; }
  std::string toString() const override { return "unsafe { ... }"; }
  std::string dotLabel() const override { return "Unsafe"; }

  const BlockExprAST& getBody() const { return *body; }
  BlockExprAST& getBody() { return *body; }
  std::unique_ptr<ExprAST> clone() const override;
};
