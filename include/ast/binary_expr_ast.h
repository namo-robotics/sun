// binary_expr_ast.h — BinaryExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/call_expr_ast.h"
#include "ast/expr_ast.h"
#include "ast/member_access_ast.h"
#include "parsing/lexer.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::parsing::Token;

/** A binary operation that becomes an ordinary call when a class hook resolves.
 * Inheriting the call representation lets existing visitors handle lowered
 * operators without introducing separate lifetime or serialization rules. */
class BinaryExprAST : public CallExprAST {
  Token op;
  std::unique_ptr<ExprAST> LHS, RHS;

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  BinaryExprAST(Token Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : CallExprAST(nullptr, {}), op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {
    setLocation(Op.start);
  }
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return Callee ? ASTNodeType::CALL : ASTNodeType::BINARY;
  }
  /** Converts class arithmetic to an ordinary call so all later passes use
   * the same ownership, lifetime, serialization, and execution rules. */
  void lowerToMethod(const std::string& name) {
    Callee = std::make_unique<MemberAccessAST>(std::move(LHS), name);
    Callee->setLocation(getLocation());
    Args.push_back(std::move(RHS));
    inheritSourceFile(getSourceFileId());
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    if (Callee) return CallExprAST::toString();
    return "(" + LHS->toString() + " " + op.text + " " + RHS->toString() + ")";
  }
  /** Returns the operator applied by this expression. */
  Token getOp() const { return op; }
  /** Returns the left operand. */
  const ExprAST* getLHS() const { return LHS.get(); }
  /** Returns the right operand. */
  const ExprAST* getRHS() const { return RHS.get(); }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (Callee) {
      CallExprAST::forEachChildSlot(fn);
      return;
    }
    fn(LHS);
    fn(RHS);
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    return Callee ? CallExprAST::dotLabel() : "Binary\n" + op.text;
  }
};

}  // namespace sun::ast
