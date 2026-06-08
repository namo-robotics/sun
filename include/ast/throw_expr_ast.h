// throw_expr_ast.h — ThrowExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Throw expression: throw <expr>
// Used to throw an error from a function declared with ", IError"
class ThrowExprAST : public ExprAST {
  std::unique_ptr<ExprAST> errorExpr;  // The error expression to throw

 public:
  explicit ThrowExprAST(std::unique_ptr<ExprAST> expr)
      : errorExpr(std::move(expr)) {}

  ASTNodeType getType() const override { return ASTNodeType::THROW; }
  std::string toString() const override {
    return "throw " + errorExpr->toString();
  }

  const ExprAST& getErrorExpr() const { return *errorExpr; }
  bool hasErrorExpr() const { return errorExpr != nullptr; }
  std::string dotLabel() const override { return "Throw"; }
  std::unique_ptr<ExprAST> clone() const override;
};
