// index_ast.h — IndexAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"
#include "ast/slice_expr_ast.h"

// Index expression: x[i] or x[i, j, k] for n-dimensional indexing
// Applies to arrays or any type implementing IIndexable
// Each index can be a single value or a slice range
// Uses comma-separated indices: x[0, 1] instead of x[0][1]
// Supports slicing: x[1:10, 3:5] or mixed x[0, 1:5]
class IndexAST : public ExprAST {
  std::unique_ptr<ExprAST>
      target;  // The target being indexed (array or IIndexable)
  std::vector<std::unique_ptr<SliceExprAST>>
      indices;  // One or more index/slice components

 public:
  IndexAST(std::unique_ptr<ExprAST> target,
           std::vector<std::unique_ptr<SliceExprAST>> idxs)
      : target(std::move(target)), indices(std::move(idxs)) {}
  ASTNodeType getType() const override { return ASTNodeType::INDEX; }
  std::string toString() const override {
    std::string result = target->toString() + "[";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i > 0) result += ", ";
      result += indices[i]->toString();
    }
    return result + "]";
  }
  const ExprAST* getTarget() const { return target.get(); }
  const std::vector<std::unique_ptr<SliceExprAST>>& getIndices() const {
    return indices;
  }
  size_t numIndices() const { return indices.size(); }

  // Check if any index component is a range slice
  bool hasSlices() const {
    for (const auto& idx : indices) {
      if (idx->isRange()) return true;
    }
    return false;
  }
  std::string dotLabel() const override { return "Index"; }
};
