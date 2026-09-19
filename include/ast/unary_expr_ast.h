// unary_expr_ast.h — UnaryExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"
#include "parsing/lexer.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::parsing::Token;

/** An operation applied to a single operand expression. */
class UnaryExprAST : public ExprAST {
  Token op;
  std::unique_ptr<ExprAST> Operand;

 public:
  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override { fn(Operand); }

  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  UnaryExprAST(Token op, std::unique_ptr<ExprAST> operand)
      : ExprAST(op.start), op(op), Operand(std::move(operand)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::UNARY; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return op.text + Operand->toString();
  }
  /** Returns the operator applied by this expression. */
  Token getOp() const { return op; }
  /** Returns the operand stored by this object. */
  const ExprAST* getOperand() const { return Operand.get(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Unary\n" + op.text; }
};

}  // namespace sun::ast
