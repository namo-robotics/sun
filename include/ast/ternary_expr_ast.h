// ternary_expr_ast.h — TernaryExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Ternary conditional: cond ? thenExpr : elseExpr. Always value-producing;
 * branch types are unified by the semantic analyzer (exact match, integer
 * literal coercion, or numeric widening).
 */
class TernaryExprAST : public ExprAST {
  std::unique_ptr<ExprAST> cond, thenExpr, elseExpr;

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  TernaryExprAST(std::unique_ptr<ExprAST> cond,
                 std::unique_ptr<ExprAST> thenExpr,
                 std::unique_ptr<ExprAST> elseExpr, sun::support::Position loc)
      : ExprAST(loc),
        cond(std::move(cond)),
        thenExpr(std::move(thenExpr)),
        elseExpr(std::move(elseExpr)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::TERNARY; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(cond);
    fn(thenExpr);
    fn(elseExpr);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return cond->toString() + " ? " + thenExpr->toString() + " : " +
           elseExpr->toString();
  }
  /** Returns the condition expression. */
  ExprAST* getCond() const { return cond.get(); }
  /** Returns the branch taken when the condition is true. */
  ExprAST* getThen() const { return thenExpr.get(); }
  /** Returns the alternative branch. */
  ExprAST* getElse() const { return elseExpr.get(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Ternary\n?:"; }
};

}  // namespace sun::ast
