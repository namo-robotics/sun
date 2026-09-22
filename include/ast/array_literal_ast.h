// array_literal_ast.h — ArrayLiteralAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Array literal: [1, 2, 3] or [[1, 2], [3, 4]] for nested arrays
 */
class ArrayLiteralAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> elements;

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  explicit ArrayLiteralAST(std::vector<std::unique_ptr<ExprAST>> elems)
      : elements(std::move(elems)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::ARRAY_LITERAL; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& elem : elements) fn(elem);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = "[";
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0) result += ", ";
      result += elements[i]->toString();
    }
    return result + "]";
  }
  /** Returns the elements stored by this object. */
  const std::vector<std::unique_ptr<ExprAST>>& getElements() const {
    return elements;
  }
  /** Returns the number of stored entries. */
  size_t size() const { return elements.size(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "ArrayLiteral"; }
};

}  // namespace sun::ast
