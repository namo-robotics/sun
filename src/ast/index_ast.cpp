// index_ast.cpp — IndexAST clone implementation

#include "ast/index_ast.h"

std::unique_ptr<ExprAST> IndexAST::clone() const {
  std::vector<std::unique_ptr<SliceExprAST>> indicesClone;
  indicesClone.reserve(indices.size());
  for (const auto& idx : indices) {
    auto cloned = idx->clone();
    indicesClone.push_back(std::unique_ptr<SliceExprAST>(
        static_cast<SliceExprAST*>(cloned.release())));
  }
  auto copy =
      std::make_unique<IndexAST>(target->clone(), std::move(indicesClone));
  cloneBase(*copy);
  return copy;
}
