// slice_expr_ast.h — SliceExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Slice expression: represents either a single index or a range slice
// Single index: x[5] -> start=5, end=nullptr, isRange=false
// Range slice: x[5:10] -> start=5, end=10, isRange=true
// Partial slices: x[:10], x[5:], x[:] -> isRange=true with nullptr bounds
class SliceExprAST : public ExprAST {
  std::unique_ptr<ExprAST> start_;  // Start index (nullptr = from beginning)
  std::unique_ptr<ExprAST> end_;    // End index (nullptr = to end)
  bool isRange_;                    // true for slice (a:b), false for index (a)

 public:
  // Constructor for single index (isRange=false)
  explicit SliceExprAST(std::unique_ptr<ExprAST> index)
      : start_(std::move(index)), end_(nullptr), isRange_(false) {}

  // Constructor for range slice (isRange=true)
  SliceExprAST(std::unique_ptr<ExprAST> start, std::unique_ptr<ExprAST> end,
               bool isRange)
      : start_(std::move(start)), end_(std::move(end)), isRange_(isRange) {}

  ASTNodeType getType() const override { return ASTNodeType::SLICE; }
  std::string toString() const override {
    if (!isRange_) return start_ ? start_->toString() : "";
    std::string result = start_ ? start_->toString() : "";
    result += ":";
    if (end_) result += end_->toString();
    return result;
  }

  const ExprAST* getStart() const { return start_.get(); }
  const ExprAST* getEnd() const { return end_.get(); }
  bool isRange() const { return isRange_; }
  bool hasStart() const { return start_ != nullptr; }
  bool hasEnd() const { return end_ != nullptr; }
  std::string dotLabel() const override { return "Slice\n" + toString(); }
};
