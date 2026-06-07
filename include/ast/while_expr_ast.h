// while_expr_ast.h — WhileExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

class WhileExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Condition, Body;

 public:
  WhileExprAST(std::unique_ptr<ExprAST> Condition,
               std::unique_ptr<ExprAST> Body)
      : Condition(std::move(Condition)), Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::WHILE_LOOP; }
  std::string toString() const override {
    return "while (" + Condition->toString() + ") " + Body->toString();
  }

  const ExprAST* getCondition() const { return Condition.get(); }
  const ExprAST* getBody() const { return Body.get(); }
  std::string dotLabel() const override { return "While"; }
  std::unique_ptr<ExprAST> clone() const override;
};
