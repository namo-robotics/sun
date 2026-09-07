// number_expr_ast.h — NumberExprAST class

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "ast/expr_ast.h"

// A numeric literal. An integer literal is kept as a magnitude and a sign
// rather than a signed 64-bit value so the whole u64 range is representable:
// 18446744073709551615 has no int64_t form, yet it is a valid u64 literal. The
// sign is set only when a minus was folded into a suffixed literal (-128i8).
class NumberExprAST : public ExprAST {
 public:
  // An enum rather than a bool so a suffix string can never be mistaken for
  // the sign at a construction site.
  enum class Sign { Positive, Negative };

  struct IntegerValue {
    uint64_t magnitude;
    bool negative;
  };

 private:
  std::variant<IntegerValue, double> value_;
  // Type suffix written on the literal ("u8", "f32"); empty when untyped. A
  // suffixed literal is a typed value: it never adapts to context.
  std::string suffix_;

 public:
  NumberExprAST(uint64_t magnitude, Sign sign, std::string suffix = "")
      : value_(
            IntegerValue{magnitude, sign == Sign::Negative && magnitude != 0}),
        suffix_(std::move(suffix)) {}
  explicit NumberExprAST(int64_t intVal)
      : NumberExprAST(intVal < 0 ? uint64_t(0) - static_cast<uint64_t>(intVal)
                                 : static_cast<uint64_t>(intVal),
                      intVal < 0 ? Sign::Negative : Sign::Positive) {}
  explicit NumberExprAST(double floatVal, std::string suffix = "")
      : value_(floatVal), suffix_(std::move(suffix)) {}
  ASTNodeType getType() const override { return ASTNodeType::NUMBER; }
  std::string toString() const override {
    if (isInteger()) return getIntegerText() + suffix_;
    return std::to_string(getFloatVal()) + suffix_;
  }
  std::string dotLabel() const override { return "Number\n" + toString(); }

  bool isInteger() const {
    return std::holds_alternative<IntegerValue>(value_);
  }
  bool isFloat() const { return std::holds_alternative<double>(value_); }

  bool hasSuffix() const { return !suffix_.empty(); }
  const std::string& getSuffix() const { return suffix_; }

  // Absolute value of an integer literal.
  uint64_t getMagnitude() const {
    return std::get<IntegerValue>(value_).magnitude;
  }
  // True when the integer literal is negative; never true for a zero.
  bool isNegative() const { return std::get<IntegerValue>(value_).negative; }
  // The integer literal's two's-complement bit pattern, 64 bits wide. Every
  // integer type the literal was range-checked against reads its value from
  // the low bits of this pattern.
  uint64_t getIntegerBits() const {
    const auto& v = std::get<IntegerValue>(value_);
    return v.negative ? uint64_t(0) - v.magnitude : v.magnitude;
  }
  // The integer literal as written, without its suffix ("-129", "42").
  std::string getIntegerText() const {
    const auto& v = std::get<IntegerValue>(value_);
    return (v.negative ? "-" : "") + std::to_string(v.magnitude);
  }
  double getFloatVal() const { return std::get<double>(value_); }

  // For backward compatibility, get value as double
  double getVal() const {
    if (isInteger()) {
      double magnitude = static_cast<double>(getMagnitude());
      return isNegative() ? -magnitude : magnitude;
    }
    return std::get<double>(value_);
  }
};
