// for_expr_ast.h — ForExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** A for loop with its initialization, condition, update, and body. */
class ForExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Init;       // Initialization (can be null)
  std::unique_ptr<ExprAST> Condition;  // Condition (can be null for infinite)
  std::unique_ptr<ExprAST> Increment;  // Increment (can be null)
  std::unique_ptr<ExprAST> Body;

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  ForExprAST(std::unique_ptr<ExprAST> Init, std::unique_ptr<ExprAST> Condition,
             std::unique_ptr<ExprAST> Increment, std::unique_ptr<ExprAST> Body)
      : Init(std::move(Init)),
        Condition(std::move(Condition)),
        Increment(std::move(Increment)),
        Body(std::move(Body)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::FOR_LOOP; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(Init);
    fn(Condition);
    fn(Increment);
    fn(Body);
  }
  /** Returns a readable representation for diagnostics and debugging. */
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

  /** Returns the loop initialization expression. */
  const ExprAST* getInit() const { return Init.get(); }
  /** Returns the condition expression. */
  const ExprAST* getCondition() const { return Condition.get(); }
  /** Returns the loop update expression. */
  const ExprAST* getIncrement() const { return Increment.get(); }
  /** Provides access to the expressions that make up the body. */
  const ExprAST* getBody() const { return Body.get(); }

  /**
   * Mutable body slot for LoweringPass block normalization
   */
  std::unique_ptr<ExprAST>& bodySlot() { return Body; }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "For"; }
};

}  // namespace sun::ast
