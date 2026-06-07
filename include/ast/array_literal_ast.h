// array_literal_ast.h — ArrayLiteralAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

// Array literal: [1, 2, 3] or [[1, 2], [3, 4]] for nested arrays
class ArrayLiteralAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> elements;

 public:
  explicit ArrayLiteralAST(std::vector<std::unique_ptr<ExprAST>> elems)
      : elements(std::move(elems)) {}
  ASTNodeType getType() const override { return ASTNodeType::ARRAY_LITERAL; }
  std::string toString() const override {
    std::string result = "[";
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0) result += ", ";
      result += elements[i]->toString();
    }
    return result + "]";
  }
  const std::vector<std::unique_ptr<ExprAST>>& getElements() const {
    return elements;
  }
  size_t size() const { return elements.size(); }
  std::string dotLabel() const override { return "ArrayLiteral"; }
  std::unique_ptr<ExprAST> clone() const override;
};
