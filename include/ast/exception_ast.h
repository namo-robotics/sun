/** Represents bounded exceptions and handlers without introducing result checks. */
#pragma once

#include "ast/block_expr_ast.h"
#include "ast/variable_creation_ast.h"

/** Defines source nodes for native exception propagation. */
namespace sun::ast {
/** Transfers an exception value, or rethrows the current catch when empty. */
class ThrowAST : public ExprAST {
 public:
  std::unique_ptr<ExprAST> value;
  /** Takes ownership of the optional thrown expression. */
  explicit ThrowAST(std::unique_ptr<ExprAST> value) : value(std::move(value)) {}
  /** Identifies this node for compiler dispatch. */
  ASTNodeType getType() const override { return ASTNodeType::THROW; }
  /** Visits the operand for rewriting and source attribution. */
  void forEachChildSlot(const ChildSlotFn& fn) override { if (value) fn(value); }
  /** Describes the transfer for diagnostics. */
  std::string toString() const override { return "throw"; }
  /** Names the node in syntax-tree visualizations. */
  std::string dotLabel() const override { return "Throw"; }
};

/** Owns an optional typed binding and the statements handling that exception. */
struct ExceptionHandler {
  std::unique_ptr<ExprAST> binding;
  std::unique_ptr<ExprAST> body;
};

/** Runs a protected block and selects the first matching exception family. */
class TryCatchAST : public ExprAST {
 public:
  std::unique_ptr<ExprAST> body;
  std::vector<ExceptionHandler> handlers;
  /** Takes ownership of the protected block and ordered handlers. */
  TryCatchAST(std::unique_ptr<ExprAST> body,
              std::vector<ExceptionHandler> handlers)
      : body(std::move(body)), handlers(std::move(handlers)) {}
  /** Identifies this node for compiler dispatch. */
  ASTNodeType getType() const override { return ASTNodeType::TRY_CATCH; }
  /** Visits all child roots, including catch declarations. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(body);
    for (auto& handler : handlers) {
      if (handler.binding) fn(handler.binding);
      fn(handler.body);
    }
  }
  /** Describes the protected statement for diagnostics. */
  std::string toString() const override { return "try/catch"; }
  /** Names the node in syntax-tree visualizations. */
  std::string dotLabel() const override { return "TryCatch"; }
};
}  // namespace sun::ast
