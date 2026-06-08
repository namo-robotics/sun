// indexed_assignment_ast.h — IndexedAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Indexed assignment: arr[i] = value
class IndexedAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> target;  // The indexed expression (e.g., x[0])
  std::unique_ptr<ExprAST> value;   // The value to assign

 public:
  IndexedAssignmentAST(std::unique_ptr<ExprAST> target,
                       std::unique_ptr<ExprAST> value)
      : target(std::move(target)), value(std::move(value)) {}

  ASTNodeType getType() const override {
    return ASTNodeType::INDEXED_ASSIGNMENT;
  }
  std::string toString() const override {
    return target->toString() + " = " + value->toString();
  }
  const ExprAST* getTarget() const { return target.get(); }
  const ExprAST* getValue() const { return value.get(); }
  std::string dotLabel() const override { return "IndexedAssign"; }
};
