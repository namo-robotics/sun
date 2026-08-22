// compound_assignment_ast.h — CompoundAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"
#include "parsing/lexer.h"

// Compound assignment: target op= value (e.g. x += 1, obj.f *= 2,
// arr[i] |= mask). Kept as a single node through analysis and lowered in
// codegen as address-once -> load -> op -> store, so the target is
// evaluated exactly once. op is the compound token itself (e.g. '+=').
class CompoundAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> target;  // Lvalue: variable / member / index
  Token op;                         // The compound token (+=, -=, ...)
  std::unique_ptr<ExprAST> value;   // Right-hand side

 public:
  CompoundAssignmentAST(std::unique_ptr<ExprAST> target, Token op,
                        std::unique_ptr<ExprAST> value)
      : ExprAST(op.start),
        target(std::move(target)),
        op(op),
        value(std::move(value)) {}

  ASTNodeType getType() const override {
    return ASTNodeType::COMPOUND_ASSIGNMENT;
  }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(target);
    fn(value);
  }
  std::string toString() const override {
    return target->toString() + " " + op.text + " " + value->toString();
  }
  const ExprAST* getTarget() const { return target.get(); }
  const ExprAST* getValue() const { return value.get(); }
  const Token& getOp() const { return op; }

  // The underlying binary operator (e.g. PLUS for PLUS_ASSIGN)
  TokenKind binaryOpKind() const {
    auto binOp = compoundToBinaryOp(op.kind);
    return binOp ? *binOp : TokenKind::UNKNOWN;
  }

  std::string dotLabel() const override { return "CompoundAssign\n" + op.text; }
};
