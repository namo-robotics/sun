// array_index_ast.h — ArrayIndexAST class (legacy n-dimensional indexing)

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

// Array indexing: x[i] or x[i, j, k] for n-dimensional arrays (legacy)
// Uses comma-separated indices: x[0, 1] instead of x[0][1]
class ArrayIndexAST : public ExprAST {
  std::unique_ptr<ExprAST> array;                 // The array being indexed
  std::vector<std::unique_ptr<ExprAST>> indices;  // One or more indices

 public:
  ArrayIndexAST(std::unique_ptr<ExprAST> arr,
                std::vector<std::unique_ptr<ExprAST>> idxs)
      : array(std::move(arr)), indices(std::move(idxs)) {}
  ASTNodeType getType() const override { return ASTNodeType::ARRAY_INDEX; }
  std::string toString() const override {
    std::string result = array->toString() + "[";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i > 0) result += ", ";
      result += indices[i]->toString();
    }
    return result + "]";
  }
  const ExprAST* getArray() const { return array.get(); }
  const std::vector<std::unique_ptr<ExprAST>>& getIndices() const {
    return indices;
  }
  size_t numIndices() const { return indices.size(); }
  std::string dotLabel() const override { return "ArrayIndex"; }
};
