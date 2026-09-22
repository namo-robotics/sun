// string_literal_ast.h — StringLiteralAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** A string constant and its source spelling. */
class StringLiteralAST : public ExprAST {
  std::string Value;

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  explicit StringLiteralAST(std::string Value) : Value(std::move(Value)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::STRING_LITERAL; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "\"" + Value + "\""; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "String\n" + toString(); }
  /** Returns the value represented by this object. */
  const std::string& getValue() const { return Value; }
};

}  // namespace sun::ast
