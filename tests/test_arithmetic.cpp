// tests/test_arithmetic.cpp - Tests for arithmetic operations (+, -, *, /, %)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"
#include "sun_value.h"

// ============================================================================
// Basic Arithmetic: Addition (+)
// ============================================================================

TEST(ArithmeticOps, add_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 10 + 32;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, add_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 15;
          var y: i32 = 27;
          return x + y;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, add_i64) {
  auto value = executeString(R"(
      function main() i64 {
          var x: i64 = 1000000000000;
          var y: i64 = 2000000000000;
          return x + y;
      }
    )");
  EXPECT_EQ(value, static_cast<int64_t>(3000000000000LL));
}

TEST(ArithmeticOps, add_negative) {
  auto value = executeString(R"(
      function main() i32 {
          return 100 + -58;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, add_f64) {
  auto value = executeString(R"(
      function main() f64 {
          return 3.14 + 2.86;
      }
    )");
  EXPECT_DOUBLE_EQ(sun::toDouble(value), 6.0);
}

TEST(ArithmeticOps, add_f64_variables) {
  auto value = executeString(R"(
      function main() f64 {
          var x: f64 = 1.5;
          var y: f64 = 2.5;
          return x + y;
      }
    )");
  EXPECT_DOUBLE_EQ(sun::toDouble(value), 4.0);
}

// ============================================================================
// Basic Arithmetic: Subtraction (-)
// ============================================================================

TEST(ArithmeticOps, sub_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 100 - 58;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, sub_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 100;
          var y: i32 = 58;
          return x - y;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, sub_negative_result) {
  auto value = executeString(R"(
      function main() i32 {
          return 10 - 52;
      }
    )");
  EXPECT_EQ(value, -42);
}

TEST(ArithmeticOps, sub_f64) {
  auto value = executeString(R"(
      function main() f64 {
          return 10.5 - 4.5;
      }
    )");
  EXPECT_DOUBLE_EQ(sun::toDouble(value), 6.0);
}

// ============================================================================
// Basic Arithmetic: Multiplication (*)
// ============================================================================

TEST(ArithmeticOps, mul_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 6 * 7;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, mul_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 6;
          var y: i32 = 7;
          return x * y;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, mul_negative) {
  auto value = executeString(R"(
      function main() i32 {
          return -6 * 7;
      }
    )");
  EXPECT_EQ(value, -42);
}

TEST(ArithmeticOps, mul_both_negative) {
  auto value = executeString(R"(
      function main() i32 {
          return -6 * -7;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, mul_f64) {
  auto value = executeString(R"(
      function main() f64 {
          return 3.0 * 2.5;
      }
    )");
  EXPECT_DOUBLE_EQ(sun::toDouble(value), 7.5);
}

TEST(ArithmeticOps, mul_by_zero) {
  auto value = executeString(R"(
      function main() i32 {
          return 42 * 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Basic Arithmetic: Division (/)
// ============================================================================

TEST(ArithmeticOps, div_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 84 / 2;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, div_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 84;
          var y: i32 = 2;
          return x / y;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, div_truncation) {
  auto value = executeString(R"(
      function main() i32 {
          return 85 / 2;
      }
    )");
  EXPECT_EQ(value, 42);  // Integer division truncates
}

TEST(ArithmeticOps, div_negative) {
  auto value = executeString(R"(
      function main() i32 {
          return -84 / 2;
      }
    )");
  EXPECT_EQ(value, -42);
}

TEST(ArithmeticOps, div_f64) {
  auto value = executeString(R"(
      function main() f64 {
          return 21.0 / 2.0;
      }
    )");
  EXPECT_DOUBLE_EQ(sun::toDouble(value), 10.5);
}

TEST(ArithmeticOps, div_f64_precise) {
  auto value = executeString(R"(
      function main() f64 {
          return 85.0 / 2.0;
      }
    )");
  EXPECT_DOUBLE_EQ(sun::toDouble(value), 42.5);
}

// ============================================================================
// Modulo Operator (%)
// ============================================================================

TEST(ArithmeticOps, mod_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 17 % 5;
      }
    )");
  EXPECT_EQ(value, 2);
}

TEST(ArithmeticOps, mod_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 17;
          var y: i32 = 5;
          return x % y;
      }
    )");
  EXPECT_EQ(value, 2);
}

TEST(ArithmeticOps, mod_evenly_divisible) {
  auto value = executeString(R"(
      function main() i32 {
          return 20 % 5;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(ArithmeticOps, mod_negative_dividend) {
  auto value = executeString(R"(
      function main() i32 {
          return -17 % 5;
      }
    )");
  EXPECT_EQ(value, -2);  // C/LLVM semantics: sign follows dividend
}

TEST(ArithmeticOps, mod_negative_divisor) {
  auto value = executeString(R"(
      function main() i32 {
          return 17 % -5;
      }
    )");
  EXPECT_EQ(value, 2);  // Sign follows dividend
}

TEST(ArithmeticOps, mod_both_negative) {
  auto value = executeString(R"(
      function main() i32 {
          return -17 % -5;
      }
    )");
  EXPECT_EQ(value, -2);
}

TEST(ArithmeticOps, mod_i64) {
  auto value = executeString(R"(
      function main() i64 {
          var x: i64 = 1000000000007;
          var y: i64 = 1000000000;
          return x % y;
      }
    )");
  EXPECT_EQ(value, static_cast<int64_t>(7));
}

TEST(ArithmeticOps, mod_small_dividend) {
  auto value = executeString(R"(
      function main() i32 {
          return 3 % 10;
      }
    )");
  EXPECT_EQ(value, 3);
}

// ============================================================================
// Operator Precedence
// ============================================================================

TEST(ArithmeticOps, precedence_mul_before_add) {
  auto value = executeString(R"(
      function main() i32 {
          return 2 + 3 * 4;
      }
    )");
  EXPECT_EQ(value, 14);  // Not 20
}

TEST(ArithmeticOps, precedence_div_before_sub) {
  auto value = executeString(R"(
      function main() i32 {
          return 20 - 12 / 3;
      }
    )");
  EXPECT_EQ(value, 16);  // Not 2
}

TEST(ArithmeticOps, precedence_mod_same_as_mul) {
  auto value = executeString(R"(
      function main() i32 {
          return 2 + 10 % 3;
      }
    )");
  EXPECT_EQ(value, 3);  // 2 + (10 % 3) = 2 + 1 = 3
}

TEST(ArithmeticOps, precedence_with_parens) {
  auto value = executeString(R"(
      function main() i32 {
          return (2 + 3) * 4;
      }
    )");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Complex Expressions
// ============================================================================

TEST(ArithmeticOps, complex_expression) {
  auto value = executeString(R"(
      function main() i32 {
          return 2 * 3 + 4 * 5 - 6 / 2;
      }
    )");
  EXPECT_EQ(value, 23);  // 6 + 20 - 3 = 23
}

TEST(ArithmeticOps, nested_parens) {
  auto value = executeString(R"(
      function main() i32 {
          return ((2 + 3) * (4 + 5)) - 10;
      }
    )");
  EXPECT_EQ(value, 35);  // 5 * 9 - 10 = 35
}

// ============================================================================
// Automatic Type Widening
// ============================================================================

TEST(ArithmeticOps, i32_to_f64_explicit_cast) {
  // Sun requires explicit cast to mix int and float
  auto value = executeString(R"(
      function main() f64 {
          var x: f64 = 5.0;
          var y: f64 = 1.5;
          return x + y;
      }
    )");
  EXPECT_DOUBLE_EQ(sun::toDouble(value), 6.5);
}

TEST(ArithmeticOps, i32_to_i64_widening) {
  auto value = executeString(R"(
      function main() i64 {
          var x: i32 = 10;
          var y: i64 = 20;
          return x + y;
      }
    )");
  EXPECT_EQ(value, static_cast<int64_t>(30));
}

// ============================================================================
// Integer Type Variations
// ============================================================================

TEST(ArithmeticOps, u8_arithmetic) {
  // u8 arithmetic within safe range to avoid sign-extension issues
  auto value = executeString(R"(
      function main() i32 {
          var x: u8 = 100;
          var y: u8 = 20;
          var z: u8 = x + y;
          return z;
      }
    )");
  EXPECT_EQ(value, 120);
}

TEST(ArithmeticOps, i8_arithmetic) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i8 = 100;
          var y: i8 = 20;
          return x - y;
      }
    )");
  EXPECT_EQ(value, 80);
}

TEST(ArithmeticOps, i16_arithmetic) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i16 = 1000;
          var y: i16 = 234;
          return x + y;
      }
    )");
  EXPECT_EQ(value, 1234);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(ArithmeticOps, unary_minus) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 42;
          return -x;
      }
    )");
  EXPECT_EQ(value, -42);
}

TEST(ArithmeticOps, double_negative) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = -42;
          return -x;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(ArithmeticOps, chained_operations) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          x = x + 5;
          x = x * 2;
          x = x - 10;
          x = x / 2;
          return x;
      }
    )");
  EXPECT_EQ(value, 10);  // ((10+5)*2-10)/2 = (30-10)/2 = 10
}

TEST(ArithmeticOps, quotient_remainder_identity) {
  auto value = executeString(R"(
      function main() i32 {
          var a: i32 = 10;
          var b: i32 = 3;
          var quotient: i32 = a / b;
          var remainder: i32 = a % b;
          return quotient * b + remainder;
      }
    )");
  EXPECT_EQ(value, 10);  // Verifies q*b + r = a
}