// if_expr_ast.h — IfExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** A conditional expression with a condition and alternative branches. */
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

 public:
  /** Creates this syntax node from its operands and declaration information. */
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
      : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::IF; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = "if (" + Cond->toString() + ") " + Then->toString();
    if (Else) result += " else " + Else->toString();
    return result;
  }
  /** Returns the cond stored by this object. */
  ExprAST* getCond() const { return Cond.get(); }
  /** Returns the then stored by this object. */
  ExprAST* getThen() const { return Then.get(); }
  /** Returns the else stored by this object. */
  ExprAST* getElse() const { return Else.get(); }

  /**
   * Mutable body slots for LoweringPass block normalization
   */
  std::unique_ptr<ExprAST>& thenSlot() { return Then; }
  /** Provides the replaceable slot for the alternative branch. */
  std::unique_ptr<ExprAST>& elseSlot() { return Else; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(Cond);
    fn(Then);
    fn(Else);
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "If"; }
};

}  // namespace sun::ast
