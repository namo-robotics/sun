// while_expr_ast.h — WhileExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** A loop that repeats its body while its condition is true. */
class WhileExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Condition, Body;

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  WhileExprAST(std::unique_ptr<ExprAST> Condition,
               std::unique_ptr<ExprAST> Body)
      : Condition(std::move(Condition)), Body(std::move(Body)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::WHILE_LOOP; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(Condition);
    fn(Body);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return "while (" + Condition->toString() + ") " + Body->toString();
  }

  /** Returns the condition expression. */
  const ExprAST* getCondition() const { return Condition.get(); }
  /** Provides access to the expressions that make up the body. */
  const ExprAST* getBody() const { return Body.get(); }

  /**
   * Mutable body slot for LoweringPass block normalization
   */
  std::unique_ptr<ExprAST>& bodySlot() { return Body; }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "While"; }
};

}  // namespace sun::ast
