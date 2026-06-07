// array_literal_ast.cpp — ArrayLiteralAST clone implementation

#include "ast/array_literal_ast.h"

#include <vector>

// Helper to clone a vector of unique_ptr<ExprAST>
static std::vector<std::unique_ptr<ExprAST>> cloneExprVector(
    const std::vector<std::unique_ptr<ExprAST>>& vec) {
  std::vector<std::unique_ptr<ExprAST>> result;
  result.reserve(vec.size());
  for (const auto& expr : vec) {
    result.push_back(expr->clone());
  }
  return result;
}

std::unique_ptr<ExprAST> ArrayLiteralAST::clone() const {
  auto copy = std::make_unique<ArrayLiteralAST>(cloneExprVector(elements));
  cloneBase(*copy);
  return copy;
}
