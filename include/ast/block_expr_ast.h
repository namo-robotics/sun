// block_expr_ast.h — BlockExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

// Which construct a block is the body of. In the language, only two kinds
// evaluate to their last statement — a match arm's body and an unsafe
// block's body. Every other kind is a statement body whose trailing
// expression is not a value; in value position, `{ ... }` always means a
// struct literal, never a block. The one exception is Value, which has no
// syntax at all: it is what the compiler's own lowerings make when they need
// a block that carries a value (string interpolation).
enum class BlockKind {
  Anonymous,  // a bare block; the default for synthesized statement blocks
  MatchArm,   // the body of a match arm
  Unsafe,     // the body of `unsafe { }`
  Function,   // function, method, or lambda body
  If,         // then / else arm
  Loop,       // for / for-in / while body
  Try,        // the body of `try`
  Catch,      // a catch clause's body
  Module,     // module, file, or moon contents
  Value,      // compiler-made value block; not writable in source
};

class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Body;
  BlockKind Kind = BlockKind::Anonymous;

 public:
  BlockExprAST() = default;

  explicit BlockExprAST(std::vector<std::unique_ptr<ExprAST>> body,
                        BlockKind kind = BlockKind::Anonymous)
      : Body(std::move(body)), Kind(kind) {}

  BlockKind getKind() const { return Kind; }
  void setKind(BlockKind kind) { Kind = kind; }

  // The two kinds the language lets evaluate to their last statement, plus
  // the compiler's own syntaxless value blocks
  bool producesValue() const {
    return Kind == BlockKind::MatchArm || Kind == BlockKind::Unsafe ||
           Kind == BlockKind::Value;
  }

  void addExpression(std::unique_ptr<ExprAST> expr) {
    Body.push_back(std::move(expr));
  }

  void prependExpressions(std::vector<std::unique_ptr<ExprAST>> exprs) {
    exprs.insert(exprs.end(), std::make_move_iterator(Body.begin()),
                 std::make_move_iterator(Body.end()));
    Body = std::move(exprs);
  }

  ASTNodeType getType() const override { return ASTNodeType::BLOCK; }
  std::string toString() const override { return "{ ... }"; }

  const std::vector<std::unique_ptr<ExprAST>>& getBody() const { return Body; }
  std::vector<std::unique_ptr<ExprAST>>& mutableBody() { return Body; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& stmt : Body) fn(stmt);
  }

  // Optional: convenience method to check if block is empty
  bool isEmpty() const { return Body.empty(); }

  // Optional: get the last expression (common when evaluating blocks)
  const ExprAST* getLastExpr() const {
    return Body.empty() ? nullptr : Body.back().get();
  }
  std::string dotLabel() const override { return "Block"; }
};
