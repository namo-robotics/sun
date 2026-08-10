// tests/test_compound_assignment.cpp - Tests for compound assignment
// operators (+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "execution_utils.h"
#include "parser.h"

// ============================================================================
// All ten operators on mutable locals
// ============================================================================

TEST(CompoundAssignment, all_operators_chained) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          x += 5;    // 15
          x *= 2;    // 30
          x -= 10;   // 20
          x /= 2;    // 10
          x %= 7;    // 3
          x <<= 2;   // 12
          x |= 1;    // 13
          x &= 29;   // 13
          x ^= 3;    // 14
          x >>= 1;   // 7
          return x;
      }
    )");
  EXPECT_EQ(value, 7);
}

TEST(CompoundAssignment, plus_assign_f64) {
  auto value = executeString(R"(
      function main() i32 {
          var x: f64 = 1.0;
          x += 1.5;
          if (x == 2.5) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(CompoundAssignment, rhs_is_full_expression) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          x += 1 + 2;
          return x;
      }
    )");
  EXPECT_EQ(value, 13);
}

// ============================================================================
// Unsigned semantics through compound assignment (locks in unsigned codegen)
// ============================================================================

TEST(CompoundAssignment, unsigned_div_assign) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u32 = 3000000000;
          x /= 2;
          if (x == 1500000000) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(CompoundAssignment, unsigned_shr_assign) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u32 = 3000000000;
          x >>= 1;
          if (x == 1500000000) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// For-loop increment clause
// ============================================================================

TEST(CompoundAssignment, for_loop_increment) {
  auto value = executeString(R"(
      function main() i32 {
          var total: i32 = 0;
          for (var i: i32 = 0; i < 10; i += 2) {
              total += i;
          }
          return total;
      }
    )");
  EXPECT_EQ(value, 20);  // 0+2+4+6+8
}

// ============================================================================
// Member and this.field targets
// ============================================================================

TEST(CompoundAssignment, this_field_in_method) {
  auto value = executeString(R"(
      class Counter {
          var count: i32;
          function init() void {
              this.count = 0;
          }
          function add(n: i32) void {
              this.count += n;
          }
      }
      function main() i32 {
          var c: Counter = Counter();
          c.add(3);
          c.add(4);
          return c.count;
      }
    )");
  EXPECT_EQ(value, 7);
}

TEST(CompoundAssignment, member_target) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          function init() void {
              this.x = 5;
          }
      }
      function main() i32 {
          var p: Point = Point();
          p.x *= 4;
          p.x += 1;
          return p.x;
      }
    )");
  EXPECT_EQ(value, 21);
}

// ============================================================================
// Indexed targets
// ============================================================================

TEST(CompoundAssignment, indexed_target) {
  auto value = executeString(R"(
      function main() i32 {
          var arr: array<i32, 3> = [1, 2, 3];
          arr[0] += 10;
          arr[1] *= 5;
          arr[2] |= 4;
          return arr[0] + arr[1] + arr[2];
      }
    )");
  EXPECT_EQ(value, 28);  // 11 + 10 + 7
}

// ============================================================================
// AST shape: x += y desugars to x = x + y
// ============================================================================

TEST(CompoundAssignment, desugars_to_binary_assignment) {
  std::istringstream ss("x += y;");
  Parser parser(ss);
  parser.getNextToken();
  auto ast = parser.parseAssignmentOrExpression();

  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::VARIABLE_ASSIGNMENT);
  auto* assign = static_cast<VariableAssignmentAST*>(ast.get());
  ASSERT_EQ(assign->getValue()->getType(), ASTNodeType::BINARY);
  auto* bin = static_cast<const BinaryExprAST*>(assign->getValue());
  EXPECT_EQ(bin->getOp().kind, TokenKind::PLUS);
  EXPECT_EQ(bin->getLHS()->getType(), ASTNodeType::VARIABLE_REFERENCE);
}

// ============================================================================
// Generic-nesting lexer splits: Vec<i32>= and Vec<Vec<i32>>=
// ============================================================================

TEST(CompoundAssignment, greater_equal_split_in_generics) {
  auto value = executeString(R"(
      class Box<T> {
          var v: T;
          function init(v: T) void {
              this.v = v;
          }
      }
      function main() i32 {
          var b: Box<i32>= Box<i32>(9);
          return b.v;
      }
    )");
  EXPECT_EQ(value, 9);
}

// ============================================================================
// Errors
// ============================================================================

TEST(CompoundAssignment, not_an_expression_operator) {
  // a += b is a statement, not an expression: `x = (a += b)` must not parse
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var a: i32 = 1;
          var x: i32 = a += 2;
          return x;
      }
    )"),
                                "expected");
}
