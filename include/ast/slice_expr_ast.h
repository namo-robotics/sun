// slice_expr_ast.h — SliceExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Slice expression: represents either a single index or a range slice
 * Single index: x[5] -> start=5, end=nullptr, isRange=false
 * Range slice: x[5:10] -> start=5, end=10, isRange=true
 * Partial slices: x[:10], x[5:], x[:] -> isRange=true with nullptr bounds
 */
class SliceExprAST : public ExprAST {
  std::unique_ptr<ExprAST> start_;  // Start index (nullptr = from beginning)
  std::unique_ptr<ExprAST> end_;    // End index (nullptr = to end)
  bool isRange_;                    // true for slice (a:b), false for index (a)

 public:
  /**
   * Constructor for single index (isRange=false)
   */
  explicit SliceExprAST(std::unique_ptr<ExprAST> index)
      : start_(std::move(index)), end_(nullptr), isRange_(false) {}

  /**
   * Constructor for range slice (isRange=true)
   */
  SliceExprAST(std::unique_ptr<ExprAST> start, std::unique_ptr<ExprAST> end,
               bool isRange)
      : start_(std::move(start)), end_(std::move(end)), isRange_(isRange) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::SLICE; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(start_);
    fn(end_);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    if (!isRange_) return start_ ? start_->toString() : "";
    std::string result = start_ ? start_->toString() : "";
    result += ":";
    if (end_) result += end_->toString();
    return result;
  }

  /** Returns the start stored by this object. */
  const ExprAST* getStart() const { return start_.get(); }
  /** Returns the end stored by this object. */
  const ExprAST* getEnd() const { return end_.get(); }
  /** Reports whether this syntax node represents a slice range. */
  bool isRange() const { return isRange_; }
  /** Reports whether this object has start. */
  bool hasStart() const { return start_ != nullptr; }
  /** Reports whether this object has end. */
  bool hasEnd() const { return end_ != nullptr; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Slice\n" + toString(); }
};

}  // namespace sun::ast
