// number_expr_ast.h — NumberExprAST class

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "ast/expr_ast.h"

class NumberExprAST : public ExprAST {
  std::variant<int64_t, double> value_;

 public:
  explicit NumberExprAST(int64_t intVal) : value_(intVal) {}
  explicit NumberExprAST(double floatVal) : value_(floatVal) {}
  ASTNodeType getType() const override { return ASTNodeType::NUMBER; }
  std::string toString() const override {
    if (isInteger()) return std::to_string(getIntVal());
    return std::to_string(getFloatVal());
  }
  std::string dotLabel() const override { return "Number\n" + toString(); }
  std::unique_ptr<ExprAST> clone() const override;

  bool isInteger() const { return std::holds_alternative<int64_t>(value_); }
  bool isFloat() const { return std::holds_alternative<double>(value_); }

  int64_t getIntVal() const { return std::get<int64_t>(value_); }
  double getFloatVal() const { return std::get<double>(value_); }

  // For backward compatibility, get value as double
  double getVal() const {
    if (isInteger()) return static_cast<double>(std::get<int64_t>(value_));
    return std::get<double>(value_);
  }
};
