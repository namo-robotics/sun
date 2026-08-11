// tests/test_ternary.cpp - Tests for the ternary conditional operator
// (cond ? then : else)

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
// Basic evaluation
// ============================================================================

TEST(Ternary, true_takes_then_branch) {
  auto value = executeString(R"(
      function main() i32 {
          var c = true;
          return c ? 1 : 2;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, false_takes_else_branch) {
  auto value = executeString(R"(
      function main() i32 {
          var c = false;
          return c ? 1 : 2;
      }
    )");
  EXPECT_EQ(value, 2);
}

TEST(Ternary, float_branches) {
  auto value = executeString(R"(
      function main() i32 {
          var c = true;
          var x = c ? 1.5 : 2.5;
          if (x == 1.5) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, comparison_condition) {
  auto value = executeString(R"(
      function main() i32 {
          var n = 7;
          return n > 5 ? 10 : 20;
      }
    )");
  EXPECT_EQ(value, 10);
}

TEST(Ternary, as_var_initializer) {
  auto value = executeString(R"(
      function main() i32 {
          var c = false;
          var x = c ? 3 : 4;
          return x;
      }
    )");
  EXPECT_EQ(value, 4);
}

TEST(Ternary, as_call_argument) {
  auto value = executeString(R"(
      function twice(v: i32) i32 { return v * 2; }
      function main() i32 {
          var c = true;
          return twice(c ? 5 : 9);
      }
    )");
  EXPECT_EQ(value, 10);
}

TEST(Ternary, in_assignment_and_compound_assignment) {
  auto value = executeString(R"(
      function main() i32 {
          var c = true;
          var x: i32 = 0;
          x = c ? 10 : 20;
          x += c ? 1 : 2;
          return x;
      }
    )");
  EXPECT_EQ(value, 11);
}

// ============================================================================
// Non-bool conditions (same laxness as if/while)
// ============================================================================

TEST(Ternary, nonzero_integer_condition_is_true) {
  auto value = executeString(R"(
      function main() i32 {
          var n = 5;
          return n ? 1 : 2;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, zero_integer_condition_is_false) {
  auto value = executeString(R"(
      function main() i32 {
          var n = 0;
          return n ? 1 : 2;
      }
    )");
  EXPECT_EQ(value, 2);
}

// ============================================================================
// Laziness: only the taken branch is evaluated
// ============================================================================

TEST(Ternary, untaken_branch_not_evaluated) {
  auto value = executeString(R"(
      function main() i32 {
          var b = 0;
          return b != 0 ? 10 / b : 7;
      }
    )");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Associativity and precedence
// ============================================================================

TEST(Ternary, ast_shape_right_associative) {
  // a ? b : c ? d : e parses as a ? b : (c ? d : e)
  auto ast = parseStatementToAst("a ? b : c ? d : e;");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::TERNARY);
  auto* outer = static_cast<TernaryExprAST*>(ast.get());
  EXPECT_EQ(outer->getCond()->getType(), ASTNodeType::VARIABLE_REFERENCE);
  EXPECT_EQ(outer->getThen()->getType(), ASTNodeType::VARIABLE_REFERENCE);
  ASSERT_EQ(outer->getElse()->getType(), ASTNodeType::TERNARY);
}

TEST(Ternary, ast_shape_nested_then_branch) {
  // a ? b ? c : d : e parses as a ? (b ? c : d) : e
  auto ast = parseStatementToAst("a ? b ? c : d : e;");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::TERNARY);
  auto* outer = static_cast<TernaryExprAST*>(ast.get());
  ASSERT_EQ(outer->getThen()->getType(), ASTNodeType::TERNARY);
  EXPECT_EQ(outer->getElse()->getType(), ASTNodeType::VARIABLE_REFERENCE);
}

TEST(Ternary, ast_shape_or_binds_tighter) {
  // x or y ? 1 : 2 parses as (x or y) ? 1 : 2
  auto ast = parseStatementToAst("x or y ? 1 : 2;");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::TERNARY);
  auto* ternary = static_cast<TernaryExprAST*>(ast.get());
  EXPECT_EQ(ternary->getCond()->getType(), ASTNodeType::BINARY);
}

TEST(Ternary, chained_runtime) {
  auto value = executeString(R"(
      function sign(n: i32) i32 {
          return n < 0 ? 0 - 1 : n == 0 ? 0 : 1;
      }
      function main() i32 {
          return sign(0 - 5) + sign(0) * 10 + sign(8) * 100;
      }
    )");
  EXPECT_EQ(value, 99);
}

TEST(Ternary, arithmetic_branches) {
  auto value = executeString(R"(
      function main() i32 {
          var c = false;
          return c ? 1 + 2 : 3 * 4;
      }
    )");
  EXPECT_EQ(value, 12);
}

TEST(Ternary, logical_condition_runtime) {
  auto value = executeString(R"(
      function main() i32 {
          var x = false;
          var y = true;
          return x or y ? 1 : 2;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Branch type unification
// ============================================================================

TEST(Ternary, integer_literal_adopts_i64) {
  auto value = executeString(R"(
      function main() i32 {
          var a: i64 = 500;
          var c = true;
          var r = c ? a : 0;
          if (r == 500) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, integer_literal_adopts_i64_reversed) {
  auto value = executeString(R"(
      function main() i32 {
          var a: i64 = 500;
          var c = false;
          var r = c ? 0 : a;
          if (r == 500) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, widens_i32_to_i64) {
  auto value = executeString(R"(
      function main() i32 {
          var a: i32 = 3;
          var b: i64 = 4000000000;
          var c = false;
          var r = c ? a : b;
          if (r == 4000000000) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, widens_i32_to_i64_taken_narrow_side) {
  auto value = executeString(R"(
      function main() i32 {
          var a: i32 = 3;
          var b: i64 = 4000000000;
          var c = true;
          var r = c ? a : b;
          if (r == 3) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, widens_f32_to_f64) {
  auto value = executeString(R"(
      function main() i32 {
          var a: f32 = 1.5;
          var b: f64 = 2.5;
          var c = true;
          var r = c ? a : b;
          if (r == 1.5) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Ternary, incompatible_branch_types_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var s = "hi";
          var c = true;
          var r = c ? 1 : s;
          return 0;
      }
    )"),
                                "Ternary branch types do not match");
}

// ============================================================================
// Class-typed branches
// ============================================================================

TEST(Ternary, class_branches_pick_field) {
  auto value = executeString(R"(
      class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) {
          this.x = x;
          this.y = y;
        }
      }

      function main() i32 {
          var a = Point(1, 2);
          var b = Point(3, 4);
          var c = false;
          var r = c ? a : b;
          return r.x;
      }
    )");
  EXPECT_EQ(value, 3);
}

// ============================================================================
// Interaction with array indexing and slices
// ============================================================================

TEST(Ternary, as_array_index) {
  auto value = executeString(R"(
      function main() i32 {
          var arr = [10, 20, 30];
          var c = false;
          return arr[c ? 0 : 1];
      }
    )");
  EXPECT_EQ(value, 20);
}

TEST(Ternary, slice_syntax_still_parses) {
  // arr[1:3] must still parse as a slice, not a ternary
  auto ast = parseStatementToAst("arr[1:3];");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::INDEX);
  auto* index = static_cast<IndexAST*>(ast.get());
  ASSERT_EQ(index->getIndices().size(), 1u);
  const auto& slice = index->getIndices()[0];
  ASSERT_TRUE(slice->hasStart());
  ASSERT_TRUE(slice->hasEnd());
  EXPECT_EQ(slice->getStart()->getType(), ASTNodeType::NUMBER);
  EXPECT_EQ(slice->getEnd()->getType(), ASTNodeType::NUMBER);
}

TEST(Ternary, parenthesized_ternary_as_slice_bound_parses) {
  // A ternary slice bound needs parens: arr[(c ? 1 : 0):3]
  auto ast = parseStatementToAst("arr[(c ? 1 : 0):3];");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::INDEX);
  auto* index = static_cast<IndexAST*>(ast.get());
  ASSERT_EQ(index->getIndices().size(), 1u);
  const auto& slice = index->getIndices()[0];
  ASSERT_TRUE(slice->hasStart());
  ASSERT_TRUE(slice->hasEnd());
  ASSERT_EQ(slice->getStart()->getType(), ASTNodeType::PAREN_EXPR);
  const auto* paren = static_cast<const ParenExprAST*>(slice->getStart());
  EXPECT_EQ(paren->getInner()->getType(), ASTNodeType::TERNARY);
  EXPECT_EQ(slice->getEnd()->getType(), ASTNodeType::NUMBER);
}

TEST(Ternary, unparenthesized_ternary_in_index_consumes_colon) {
  // arr[c ? 0 : 1] indexes with the ternary result (JS-consistent):
  // the ternary greedily consumes the ':', so this is a plain index
  auto ast = parseStatementToAst("arr[c ? 0 : 1];");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getType(), ASTNodeType::INDEX);
  auto* index = static_cast<IndexAST*>(ast.get());
  ASSERT_EQ(index->getIndices().size(), 1u);
  const auto& slice = index->getIndices()[0];
  ASSERT_TRUE(slice->hasStart());
  EXPECT_FALSE(slice->hasEnd());
  EXPECT_EQ(slice->getStart()->getType(), ASTNodeType::TERNARY);
}

// ============================================================================
// Generic functions (exercises re-analysis / clearResolvedTypes)
// ============================================================================

TEST(Ternary, inside_generic_function) {
  auto value = executeString(R"(
      function pick <T> (c: bool, a: T, b: T) T {
          return c ? a : b;
      }

      function main() i32 {
          var x = pick<i32>(true, 1, 2);
          var y = pick<i64>(false, 10, 20);
          if (x == 1) { if (y == 20) { return 1; } }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Errors
// ============================================================================

TEST(Ternary, missing_colon_is_parse_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var c = true;
          var x = c ? 1;
          return x;
      }
    )"),
                                "expected ':' in ternary expression");
}

TEST(Ternary, missing_else_operand_is_parse_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var c = true;
          var x = c ? 1 : ;
          return x;
      }
    )"),
                                "expect");
}
