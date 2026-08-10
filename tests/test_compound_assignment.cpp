// tests/test_compound_assignment.cpp - Tests for compound assignment
// operators (+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "execution_utils.h"
#include "parser.h"

// Parse a single assignment-or-expression statement and return its AST
static std::unique_ptr<ExprAST> parseStatementToAst(const std::string& source) {
  std::istringstream ss(source);
  Parser parser(ss);
  parser.getNextToken();
  return parser.parseAssignmentOrExpression();
}

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

TEST(CompoundAssignment, indexed_side_effect_index_evaluated_once) {
  auto value = executeString(R"(
      function next(counter: ref array<i32>) i32 {
          counter[0] += 1;
          return 1;
      }
      function main() i32 {
          var arr: array<i32, 3> = [10, 20, 30];
          var calls: array<i32, 1> = [0];
          arr[next(calls)] += 5;
          if (calls[0] == 1 and arr[1] == 25) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(CompoundAssignment, indexed_multidim_side_effect_once) {
  auto value = executeString(R"(
      function bump(counter: ref array<i32>, v: i32) i32 {
          counter[0] += 1;
          return v;
      }
      function main() i32 {
          var m: array<i32, 2, 2> = [[1, 2], [3, 4]];
          var calls: array<i32, 1> = [0];
          m[bump(calls, 1), bump(calls, 0)] *= 10;
          if (calls[0] == 2 and m[1, 0] == 30) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// AST shape: x += y desugars to x = x + y
// ============================================================================

TEST(CompoundAssignment, desugars_to_binary_assignment) {
  auto ast = parseStatementToAst("x += y;");

  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::VARIABLE_ASSIGNMENT);
  auto* assign = static_cast<VariableAssignmentAST*>(ast.get());
  ASSERT_EQ(assign->getValue()->getType(), ASTNodeType::BINARY);
  auto* bin = static_cast<const BinaryExprAST*>(assign->getValue());
  EXPECT_EQ(bin->getOp().kind, TokenKind::PLUS);
  EXPECT_EQ(bin->getLHS()->getType(), ASTNodeType::VARIABLE_REFERENCE);
}

TEST(CompoundAssignment, indexed_pure_index_stays_plain_assignment) {
  auto ast = parseStatementToAst("arr[i] += 1;");

  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(ast->getType(), ASTNodeType::INDEXED_ASSIGNMENT);
}

TEST(CompoundAssignment, indexed_call_index_hoisted_into_block) {
  auto ast = parseStatementToAst("arr[f()] += 1;");

  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::BLOCK);
  auto* block = static_cast<BlockExprAST*>(ast.get());
  ASSERT_EQ(block->getBody().size(), 2u);
  EXPECT_EQ(block->getBody()[0]->getType(), ASTNodeType::VARIABLE_CREATION);
  EXPECT_EQ(block->getBody()[1]->getType(), ASTNodeType::INDEXED_ASSIGNMENT);
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
