// return_expr_ast.h — ReturnExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Return statement: return <expr>;
class ReturnExprAST : public ExprAST {
  std::unique_ptr<ExprAST>
      Value;  // The expression to return (may be nullptr for void)

 public:
  explicit ReturnExprAST(std::unique_ptr<ExprAST> value = nullptr)
      : Value(std::move(value)) {}

  ASTNodeType getType() const override { return ASTNodeType::RETURN; }
  std::string toString() const override {
    if (Value) return "return " + Value->toString();
    return "return";
  }

  const ExprAST* getValue() const { return Value.get(); }
  bool hasValue() const { return Value != nullptr; }
  std::string dotLabel() const override { return "Return"; }
  std::unique_ptr<ExprAST> clone() const override;
};
