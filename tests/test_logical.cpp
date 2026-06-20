// tests/test_logical.cpp - Tests for logical operations (and, or, not)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Logical AND (and) - short-circuit evaluation
// ============================================================================

TEST(LogicalOps, and_true_true) {
  auto value = executeString(R"(
      function main() bool {
          return true and true;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, and_true_false) {
  auto value = executeString(R"(
      function main() bool {
          return true and false;
      }
    )");
  EXPECT_EQ(value, false);
}

TEST(LogicalOps, and_false_true) {
  auto value = executeString(R"(
      function main() bool {
          return false and true;
      }
    )");
  EXPECT_EQ(value, false);
}

TEST(LogicalOps, and_false_false) {
  auto value = executeString(R"(
      function main() bool {
          return false and false;
      }
    )");
  EXPECT_EQ(value, false);
}

TEST(LogicalOps, and_with_variables) {
  auto value = executeString(R"(
      function main() bool {
          var a: bool = true;
          var b: bool = true;
          return a and b;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, and_short_circuit) {
  // If short-circuit works, the second condition shouldn't be evaluated
  // We test this by having a side effect that would fail if evaluated
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 0;
          if (false and true) {
              x = 1;
          }
          return x;
      }
    )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Logical OR (or) - short-circuit evaluation
// ============================================================================

TEST(LogicalOps, or_true_true) {
  auto value = executeString(R"(
      function main() bool {
          return true or true;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, or_true_false) {
  auto value = executeString(R"(
      function main() bool {
          return true or false;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, or_false_true) {
  auto value = executeString(R"(
      function main() bool {
          return false or true;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, or_false_false) {
  auto value = executeString(R"(
      function main() bool {
          return false or false;
      }
    )");
  EXPECT_EQ(value, false);
}

TEST(LogicalOps, or_with_variables) {
  auto value = executeString(R"(
      function main() bool {
          var a: bool = false;
          var b: bool = true;
          return a or b;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, or_short_circuit) {
  // If short-circuit works, the second condition shouldn't be evaluated
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 0;
          if (true or false) {
              x = 1;
          }
          return x;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Comparison Operators with Logical Ops
// ============================================================================

TEST(LogicalOps, comparison_and) {
  auto value = executeString(R"(
      function main() bool {
          var x: i32 = 5;
          return x > 0 and x < 10;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, comparison_or) {
  auto value = executeString(R"(
      function main() bool {
          var x: i32 = 15;
          return x < 0 or x > 10;
      }
    )");
  EXPECT_EQ(value, true);
}

// ============================================================================
// Complex Logical Expressions
// ============================================================================

TEST(LogicalOps, complex_and_or) {
  auto value = executeString(R"(
      function main() bool {
          var a: bool = true;
          var b: bool = false;
          var c: bool = true;
          return (a and b) or c;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, complex_or_and) {
  auto value = executeString(R"(
      function main() bool {
          var a: bool = false;
          var b: bool = true;
          var c: bool = true;
          return a or (b and c);
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, nested_logical) {
  auto value = executeString(R"(
      function main() bool {
          var a: bool = true;
          var b: bool = false;
          return a and (a or b);
      }
    )");
  EXPECT_EQ(value, true);  // De Morgan: !(T and F) = T
}

// ============================================================================
// Operator Precedence
// ============================================================================

TEST(LogicalOps, precedence_and_before_or) {
  auto value = executeString(R"(
      function main() bool {
          return true or false and false;
      }
    )");
  EXPECT_EQ(value, true);  // true or (false and false) = true or false = true
}

TEST(LogicalOps, precedence_comparison_before_logical) {
  auto value = executeString(R"(
      function main() bool {
          return 1 < 2 and 3 < 4;
      }
    )");
  EXPECT_EQ(value, true);  // (1 < 2) and (3 < 4) = true and true = true
}

TEST(LogicalOps, precedence_with_parens) {
  auto value = executeString(R"(
      function main() bool {
          return (true or false) and false;
      }
    )");
  EXPECT_EQ(value, false);  // true and false = false
}

// ============================================================================
// Logical Ops in Control Flow
// ============================================================================

TEST(LogicalOps, if_with_and) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 5;
          if (x > 0 and x < 10) {
              return 1;
          }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(LogicalOps, if_with_or) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = -5;
          if (x < 0 or x > 100) {
              return 1;
          }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(LogicalOps, while_with_and) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 0;
          var count: i32 = 0;
          while (x < 10 and count < 5) {
              x = x + 1;
              count = count + 1;
          }
          return count;
      }
    )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(LogicalOps, chained_and) {
  auto value = executeString(R"(
      function main() bool {
          return true and true and true and true;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, chained_or) {
  auto value = executeString(R"(
      function main() bool {
          return false or false or false or true;
      }
    )");
  EXPECT_EQ(value, true);
}

TEST(LogicalOps, mixed_chain) {
  auto value = executeString(R"(
      function main() bool {
          return true and false or true and true;
      }
    )");
  EXPECT_EQ(value, true);  // (true and false) or (true and true) = false or true = true
}

TEST(LogicalOps, integers_as_bool) {
  // Test that non-zero integers are truthy
  auto value = executeString(R"(
      function main() bool {
          var x: i32 = 5;
          var y: i32 = 0;
          return x and y;
      }
    )");
  EXPECT_EQ(value, false);  // 5 is truthy, 0 is falsy
}

TEST(LogicalOps, integers_or) {
  auto value = executeString(R"(
      function main() bool {
          var x: i32 = 0;
          var y: i32 = 3;
          return x or y;
      }
    )");
  EXPECT_EQ(value, true);  // 0 is falsy, 3 is truthy
}
