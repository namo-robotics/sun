// throw_expr_ast.h — ThrowExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Throw expression: throw &lt;expr&gt;
 * Used to throw an error from a function declared with "throws IError"
 */
class ThrowExprAST : public ExprAST {
  std::unique_ptr<ExprAST> errorExpr;  // The error expression to throw

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  explicit ThrowExprAST(std::unique_ptr<ExprAST> expr)
      : errorExpr(std::move(expr)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::THROW; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override { fn(errorExpr); }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return "throw " + errorExpr->toString();
  }

  /** Returns the thrown error expression. */
  const ExprAST& getErrorExpr() const { return *errorExpr; }
  /** Reports whether this object has error expr. */
  bool hasErrorExpr() const { return errorExpr != nullptr; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Throw"; }
};

}  // namespace sun::ast
