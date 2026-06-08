// for_expr_ast.h — ForExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

class ForExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Init;       // Initialization (can be null)
  std::unique_ptr<ExprAST> Condition;  // Condition (can be null for infinite)
  std::unique_ptr<ExprAST> Increment;  // Increment (can be null)
  std::unique_ptr<ExprAST> Body;

 public:
  ForExprAST(std::unique_ptr<ExprAST> Init, std::unique_ptr<ExprAST> Condition,
             std::unique_ptr<ExprAST> Increment, std::unique_ptr<ExprAST> Body)
      : Init(std::move(Init)),
        Condition(std::move(Condition)),
        Increment(std::move(Increment)),
        Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::FOR_LOOP; }
  std::string toString() const override {
    std::string result = "for (";
    if (Init) result += Init->toString();
    result += "; ";
    if (Condition) result += Condition->toString();
    result += "; ";
    if (Increment) result += Increment->toString();
    result += ") " + Body->toString();
    return result;
  }

  const ExprAST* getInit() const { return Init.get(); }
  const ExprAST* getCondition() const { return Condition.get(); }
  const ExprAST* getIncrement() const { return Increment.get(); }
  const ExprAST* getBody() const { return Body.get(); }
  std::string dotLabel() const override { return "For"; }
};
