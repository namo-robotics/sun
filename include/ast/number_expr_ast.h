// number_expr_ast.h — NumberExprAST class

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "ast/expr_ast.h"

class NumberExprAST : public ExprAST {
  std::variant<int64_t, double> value_;
  // Type suffix written on the literal ("u8", "f32"); empty when untyped. A
  // suffixed literal is a typed value: it never adapts to context.
  std::string suffix_;

 public:
  explicit NumberExprAST(int64_t intVal, std::string suffix = "")
      : value_(intVal), suffix_(std::move(suffix)) {}
  explicit NumberExprAST(double floatVal, std::string suffix = "")
      : value_(floatVal), suffix_(std::move(suffix)) {}
  ASTNodeType getType() const override { return ASTNodeType::NUMBER; }
  std::string toString() const override {
    if (isInteger()) return std::to_string(getIntVal()) + suffix_;
    return std::to_string(getFloatVal()) + suffix_;
  }
  std::string dotLabel() const override { return "Number\n" + toString(); }

  bool isInteger() const { return std::holds_alternative<int64_t>(value_); }
  bool isFloat() const { return std::holds_alternative<double>(value_); }

  bool hasSuffix() const { return !suffix_.empty(); }
  const std::string& getSuffix() const { return suffix_; }

  int64_t getIntVal() const { return std::get<int64_t>(value_); }
  double getFloatVal() const { return std::get<double>(value_); }

  // For backward compatibility, get value as double
  double getVal() const {
    if (isInteger()) return static_cast<double>(std::get<int64_t>(value_));
    return std::get<double>(value_);
  }
};
