// block_expr_ast.h — BlockExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Body;

 public:
  BlockExprAST() = default;

  explicit BlockExprAST(std::vector<std::unique_ptr<ExprAST>> body)
      : Body(std::move(body)) {}

  void addExpression(std::unique_ptr<ExprAST> expr) {
    Body.push_back(std::move(expr));
  }

  ASTNodeType getType() const override { return ASTNodeType::BLOCK; }
  std::string toString() const override { return "{ ... }"; }

  const std::vector<std::unique_ptr<ExprAST>>& getBody() const { return Body; }

  // Optional: convenience method to check if block is empty
  bool isEmpty() const { return Body.empty(); }

  // Optional: get the last expression (common when evaluating blocks)
  const ExprAST* getLastExpr() const {
    return Body.empty() ? nullptr : Body.back().get();
  }
  std::string dotLabel() const override { return "Block"; }
  std::unique_ptr<ExprAST> clone() const override;
};
