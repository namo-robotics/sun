// index_ast.h — IndexAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"
#include "ast/slice_expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Index expression: x[i] or x[i, j, k] for n-dimensional indexing
 * Applies to arrays or any type implementing IIndexable
 * Each index can be a single value or a slice range
 * Uses comma-separated indices: x[0, 1] instead of x[0][1]
 * Supports slicing: x[1:10, 3:5] or mixed x[0, 1:5]
 */
class IndexAST : public ExprAST {
  std::unique_ptr<ExprAST>
      target;  // The target being indexed (array or IIndexable)
  std::vector<std::unique_ptr<SliceExprAST>>
      indices;  // One or more index/slice components

 public:
  /** Creates this syntax node from its operands and declaration information. */
  IndexAST(std::unique_ptr<ExprAST> target,
           std::vector<std::unique_ptr<SliceExprAST>> idxs)
      : target(std::move(target)), indices(std::move(idxs)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::INDEX; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = target->toString() + "[";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i > 0) result += ", ";
      result += indices[i]->toString();
    }
    return result + "]";
  }
  /** Returns the target stored by this object. */
  const ExprAST* getTarget() const { return target.get(); }
  /** Provides the index expressions used to select array elements. */
  const std::vector<std::unique_ptr<SliceExprAST>>& getIndices() const {
    return indices;
  }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(target);
    for (auto& slice : indices) {
      if (slice) slice->forEachChildSlot(fn);
    }
  }
  /** Returns the number of index expressions in this access. */
  size_t numIndices() const { return indices.size(); }

  /**
   * Check if any index component is a range slice
   */
  bool hasSlices() const {
    for (const auto& idx : indices) {
      if (idx->isRange()) return true;
    }
    return false;
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Index"; }
};

}  // namespace sun::ast
