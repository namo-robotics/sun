// block_expr_ast.h — BlockExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Which construct a block is the body of. In the language, only two kinds
 * evaluate to their last statement — a match arm's body and an unsafe
 * block's body. Every other kind is a statement body whose trailing
 * expression is not a value; in value position, `{ ... }` always means a
 * struct literal, never a block. The one exception is Value, which has no
 * syntax at all: it is what the compiler's own lowerings make when they need
 * a block that carries a value (string interpolation).
 */
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

/** An ordered sequence of expressions evaluated in a shared lexical scope. */
class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Body;
  BlockKind Kind = BlockKind::Anonymous;

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  BlockExprAST() = default;

  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  explicit BlockExprAST(std::vector<std::unique_ptr<ExprAST>> body,
                        BlockKind kind = BlockKind::Anonymous)
      : Body(std::move(body)), Kind(kind) {}

  /** Returns the type category used for semantic checks and dispatch. */
  BlockKind getKind() const { return Kind; }
  /** Updates the kind stored by this object. */
  void setKind(BlockKind kind) { Kind = kind; }

  /**
   * The two kinds the language lets evaluate to their last statement, plus
   * the compiler's own syntaxless value blocks
   */
  bool producesValue() const {
    return Kind == BlockKind::MatchArm || Kind == BlockKind::Unsafe ||
           Kind == BlockKind::Value;
  }

  /** Appends an owned expression to the block. */
  void addExpression(std::unique_ptr<ExprAST> expr) {
    Body.push_back(std::move(expr));
  }

  /** Moves expressions to the beginning of the block in their original order. */
  void prependExpressions(std::vector<std::unique_ptr<ExprAST>> exprs) {
    exprs.insert(exprs.end(), std::make_move_iterator(Body.begin()),
                 std::make_move_iterator(Body.end()));
    Body = std::move(exprs);
  }

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::BLOCK; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "{ ... }"; }

  /** Provides access to the expressions that make up the body. */
  const std::vector<std::unique_ptr<ExprAST>>& getBody() const { return Body; }
  /** Provides mutable access to the body so compiler passes can rewrite it. */
  std::vector<std::unique_ptr<ExprAST>>& mutableBody() { return Body; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& stmt : Body) fn(stmt);
  }

  /**
   * Optional: convenience method to check if block is empty
   */
  bool isEmpty() const { return Body.empty(); }

  /**
   * Optional: get the last expression (common when evaluating blocks)
   */
  const ExprAST* getLastExpr() const {
    return Body.empty() ? nullptr : Body.back().get();
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Block"; }
};

}  // namespace sun::ast
