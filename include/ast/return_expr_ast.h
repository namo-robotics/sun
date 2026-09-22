// return_expr_ast.h — ReturnExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Return statement: return &lt;expr&gt;;
 */
class ReturnExprAST : public ExprAST {
  std::unique_ptr<ExprAST>
      Value;  // The expression to return (may be nullptr for void)

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  explicit ReturnExprAST(std::unique_ptr<ExprAST> value = nullptr)
      : Value(std::move(value)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::RETURN; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    if (Value) return "return " + Value->toString();
    return "return";
  }

  /** Returns the value represented by this object. */
  const ExprAST* getValue() const { return Value.get(); }
  /** Reports whether this object has value. */
  bool hasValue() const { return Value != nullptr; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override { fn(Value); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Return"; }
};

}  // namespace sun::ast
