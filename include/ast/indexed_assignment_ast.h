// indexed_assignment_ast.h — IndexedAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Indexed assignment: arr[i] = value
 */
class IndexedAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> target;  // The indexed expression (e.g., x[0])
  std::unique_ptr<ExprAST> value;   // The value to assign

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  IndexedAssignmentAST(std::unique_ptr<ExprAST> target,
                       std::unique_ptr<ExprAST> value)
      : target(std::move(target)), value(std::move(value)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::INDEXED_ASSIGNMENT;
  }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(target);
    fn(value);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return target->toString() + " = " + value->toString();
  }
  /** Returns the target stored by this object. */
  const ExprAST* getTarget() const { return target.get(); }
  /** Returns the value represented by this object. */
  const ExprAST* getValue() const { return value.get(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "IndexedAssign"; }
};

}  // namespace sun::ast
