// variable_assignment_ast.h — VariableAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

class VariableAssignmentAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;

 public:
  explicit VariableAssignmentAST(std::string name,
                                 std::unique_ptr<ExprAST> value)
      : name(std::move(name)), value(std::move(value)) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_ASSIGNMENT;
  }
  std::string toString() const override {
    return name + " = " + value->toString();
  }
  const std::string& getName() const { return name; }
  const ExprAST* getValue() const { return value.get(); }
  std::string dotLabel() const override { return "VarAssign\n" + name; }
  std::unique_ptr<ExprAST> clone() const override;
};
