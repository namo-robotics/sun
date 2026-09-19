// binary_expr_ast.h — BinaryExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"
#include "parsing/lexer.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::parsing::Token;

/** A binary operation with an operator and two operand expressions. */
class BinaryExprAST : public ExprAST {
  Token op;
  std::unique_ptr<ExprAST> LHS, RHS;

 public:
  /** Creates this syntax node from its operands and declaration information. */
  BinaryExprAST(Token Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Op.start), op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::BINARY; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return "(" + LHS->toString() + " " + op.text + " " + RHS->toString() + ")";
  }
  /** Returns the operator applied by this expression. */
  Token getOp() const { return op; }
  /** Returns the lhs stored by this object. */
  const ExprAST* getLHS() const { return LHS.get(); }
  /** Returns the rhs stored by this object. */
  const ExprAST* getRHS() const { return RHS.get(); }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(LHS);
    fn(RHS);
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Binary\n" + op.text; }
};

}  // namespace sun::ast
