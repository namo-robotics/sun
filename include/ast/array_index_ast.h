// array_index_ast.h — ArrayIndexAST class (legacy n-dimensional indexing)

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Array indexing: x[i] or x[i, j, k] for n-dimensional arrays (legacy)
 * Uses comma-separated indices: x[0, 1] instead of x[0][1]
 */
class ArrayIndexAST : public ExprAST {
  std::unique_ptr<ExprAST> array;                 // The array being indexed
  std::vector<std::unique_ptr<ExprAST>> indices;  // One or more indices

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  ArrayIndexAST(std::unique_ptr<ExprAST> arr,
                std::vector<std::unique_ptr<ExprAST>> idxs)
      : array(std::move(arr)), indices(std::move(idxs)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::ARRAY_INDEX; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = array->toString() + "[";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i > 0) result += ", ";
      result += indices[i]->toString();
    }
    return result + "]";
  }
  /** Returns the array expression. */
  const ExprAST* getArray() const { return array.get(); }
  /** Provides the index expressions used to select array elements. */
  const std::vector<std::unique_ptr<ExprAST>>& getIndices() const {
    return indices;
  }
  /** Returns the number of index expressions in this access. */
  size_t numIndices() const { return indices.size(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "ArrayIndex"; }
};

}  // namespace sun::ast
