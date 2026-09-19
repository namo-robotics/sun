// compound_assignment_ast.h — CompoundAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"
#include "parsing/lexer.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::parsing::Token;

/**
 * Compound assignment: target op= value (e.g. x += 1, obj.f *= 2,
 * arr[i] |= mask). Kept as a single node through analysis and lowered in
 * codegen as address-once -> load -> op -> store, so the target is
 * evaluated exactly once. op is the compound token itself (e.g. '+=').
 */
class CompoundAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> target;  // Lvalue: variable / member / index
  Token op;                         // The compound token (+=, -=, ...)
  std::unique_ptr<ExprAST> value;   // Right-hand side

 public:
  /** Creates this syntax node from its operands and declaration information. */
  CompoundAssignmentAST(std::unique_ptr<ExprAST> target, Token op,
                        std::unique_ptr<ExprAST> value)
      : ExprAST(op.start),
        target(std::move(target)),
        op(op),
        value(std::move(value)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::COMPOUND_ASSIGNMENT;
  }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(target);
    fn(value);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return target->toString() + " " + op.text + " " + value->toString();
  }
  /** Returns the target stored by this object. */
  const ExprAST* getTarget() const { return target.get(); }
  /** Returns the value represented by this object. */
  const ExprAST* getValue() const { return value.get(); }
  /** Returns the operator applied by this expression. */
  const Token& getOp() const { return op; }

  /**
   * The underlying binary operator (e.g. PLUS for PLUS_ASSIGN)
   */
  sun::parsing::TokenKind binaryOpKind() const {
    auto binOp = sun::parsing::compoundToBinaryOp(op.kind);
    return binOp ? *binOp : sun::parsing::TokenKind::UNKNOWN;
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "CompoundAssign\n" + op.text; }
};

}  // namespace sun::ast
