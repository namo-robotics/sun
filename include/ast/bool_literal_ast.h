// bool_literal_ast.h — BoolLiteralAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** A boolean constant written in source code. */
class BoolLiteralAST : public ExprAST {
  bool Value;

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  explicit BoolLiteralAST(bool Value) : Value(Value) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::BOOL_LITERAL; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return Value ? "true" : "false"; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Bool\n" + toString(); }
  /** Returns the value represented by this object. */
  bool getValue() const { return Value; }
};

}  // namespace sun::ast
