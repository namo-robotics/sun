// tests/lambdas/test_ref_captures.cpp - Tests for lambda by-reference capture
// lists: lambda [ref x] (params) ret { body }

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "driver/execution_utils.h"
#include "parsing/parser.h"

// ============================================================================
// By-ref captures: mutation through the capture is visible outside
// ============================================================================

TEST(Lambdas_RefCaptures, scalar_mutation_visible) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          var addFive = lambda [ref x] () void {
              x += 5;
          };
          addFive();
          addFive();
          return x;
      }
    )");
  EXPECT_EQ(value, 20);
}

TEST(Lambdas_RefCaptures, scalar_plain_assignment_visible) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var setNine = lambda [ref x] () void {
              x = 9;
          };
          setNine();
          return x;
      }
    )");
  EXPECT_EQ(value, 9);
}

TEST(Lambdas_RefCaptures, class_field_mutation_via_capture) {
  auto value = executeString(R"(
      class Counter {
          var count: i32;
          function init() void {
              this.count = 0;
          }
      }
      function main() i32 {
          var c: Counter = Counter();
          var tick = lambda [ref c] () void {
              c.count += 1;
          };
          tick();
          tick();
          tick();
          return c.count;
      }
    )");
  EXPECT_EQ(value, 3);
}

TEST(Lambdas_RefCaptures, array_element_write_via_capture) {
  auto value = executeString(R"(
      function main() i32 {
          var arr: array<i32, 3> = [1, 2, 3];
          var bump = lambda [ref arr] () void {
              arr[1] += 10;
          };
          bump();
          return arr[1];
      }
    )");
  EXPECT_EQ(value, 12);
}

TEST(Lambdas_RefCaptures, nested_byref_of_byref) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var outer = lambda [ref x] () void {
              var inner = lambda [ref x] () void {
                  x += 100;
              };
              inner();
          };
          outer();
          return x;
      }
    )");
  EXPECT_EQ(value, 101);
}

TEST(Lambdas_RefCaptures, byref_capture_as_ref_argument) {
  auto value = executeString(R"(
      function bump(v: ref i32) void {
          v = v + 7;
      }
      function main() i32 {
          var x: i32 = 1;
          var callBump = lambda [ref x] () void {
              bump(x);
          };
          callBump();
          return x;
      }
    )");
  EXPECT_EQ(value, 8);
}

TEST(Lambdas_RefCaptures, read_only_byvalue_still_works) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 40;
          var addTwo = lambda () i32 {
              return x + 2;
          };
          return addTwo();
      }
    )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Read-only captures: lambda [const ref x]
// ============================================================================

TEST(Lambdas_RefCaptures, const_ref_capture_reads_the_original) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          var read = lambda [const ref x] () i32 { return x; };
          x = 42;
          return read();
      }
    )");
  EXPECT_EQ(value, 42);
}

// Shared loans coexist, so two lambdas can read the same variable
TEST(Lambdas_RefCaptures, two_const_ref_captures_of_one_variable) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 20;
          var a = lambda [const ref x] () i32 { return x; };
          var b = lambda [const ref x] () i32 { return x + 2; };
          return a() + b();
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(Lambdas_RefCaptures, const_ref_capture_of_a_class_is_read_only) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Point {
          public var x: i32;
          public function init() { this.x = 1; }
      }
      function main() i32 {
          var p = Point();
          var f = lambda [const ref p] () i32 {
              p.x = 5;
              return p.x;
          };
          return f();
      }
    )"),
                                "Cannot assign to field 'x' of constant 'p'");
}

TEST(Lambdas_RefCaptures, const_ref_capture_cannot_be_assigned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [const ref x] () i32 {
              x = 2;
              return x;
          };
          return f();
      }
    )"),
                                "Cannot assign to constant 'x'");
}

TEST(Lambdas_RefCaptures, const_without_ref_is_a_parse_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [const x] () i32 { return x; };
          return f();
      }
    )"),
                                "expected 'ref' after 'const'");
}

// ============================================================================
// Rejected: mutation of by-value captures
// ============================================================================

TEST(Lambdas_RefCaptures, byvalue_mutation_rejected_with_hint) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          var f = lambda () void {
              x += 5;
          };
          f();
          return x;
      }
    )"),
      "capture it by reference with 'lambda [ref x]'");
}

TEST(Lambdas_RefCaptures, byvalue_plain_assignment_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          var f = lambda () void {
              x = 15;
          };
          f();
          return x;
      }
    )"),
                                "Cannot mutate by-value captured variable");
}

// ============================================================================
// Rejected: by-value capture of compound types
// ============================================================================

TEST(Lambdas_RefCaptures, byvalue_class_capture_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Point {
          var x: i32;
          function init() void {
              this.x = 1;
          }
      }
      function main() i32 {
          var p: Point = Point();
          var f = lambda () i32 {
              return p.x;
          };
          return f();
      }
    )"),
                                "capture it by reference with");
}

TEST(Lambdas_RefCaptures, byvalue_array_capture_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var arr: array<i32, 2> = [1, 2];
          var f = lambda () i32 {
              return arr[0];
          };
          return f();
      }
    )"),
                                "capture it by reference with");
}

TEST(Lambdas_RefCaptures, nested_function_compound_capture_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Point {
          var x: i32;
          function init() void {
              this.x = 1;
          }
      }
      function main() i32 {
          var p: Point = Point();
          function inner() i32 {
              return p.x;
          }
          return inner();
      }
    )"),
                                "use a lambda with a [ref p] capture list");
}

// ============================================================================
// Rejected: invalid capture lists
// ============================================================================

TEST(Lambdas_RefCaptures, unknown_name_in_capture_list) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [ref nosuch] () i32 {
              return x;
          };
          return f();
      }
    )"),
                                "does not use it");
}

TEST(Lambdas_RefCaptures, global_in_capture_list_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      var g: i32 = 1;
      function main() i32 {
          var f = lambda [ref g] () i32 {
              return g;
          };
          return f();
      }
    )"),
                                "globals are accessed directly");
}

TEST(Lambdas_RefCaptures, nested_byref_of_byvalue_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var outer = lambda () i32 {
              var inner = lambda [ref x] () void {
                  x += 1;
              };
              inner();
              return x;
          };
          return outer();
      }
    )"),
                                "the enclosing lambda captures it by value");
}

// ============================================================================
// Escape rules: spawn is scoped, return is not allowed
// ============================================================================

// Issue #122: a spawned thread is joined when its handle's scope ends, so a
// by-ref capture cannot outlive what it points at
TEST(Lambdas_RefCaptures, spawn_byref_lambda_accepted) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 0;
          var t = spawn(lambda [ref x] () i32 {
              x = 7;
              return 0;
          });
          var r = t.join();
          return x;
      }
    )");
  EXPECT_EQ(value, 7);
}

TEST(Lambdas_RefCaptures, spawn_byref_lambda_via_variable_accepted) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 0;
          var f = lambda [ref x] () i32 {
              x = 9;
              return 0;
          };
          var t = spawn(f);
          var r = t.join();
          return x;
      }
    )");
  EXPECT_EQ(value, 9);
}

TEST(Lambdas_RefCaptures, return_byref_lambda_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function make() () i32 {
          var x: i32 = 5;
          return lambda [ref x] () i32 {
              return x;
          };
      }
      function main() i32 {
          return 0;
      }
    )"),
                                "Borrow check failed");
}

TEST(Lambdas_RefCaptures, return_byref_lambda_via_variable_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function make() () i32 {
          var x: i32 = 5;
          var f = lambda [ref x] () i32 {
              return x;
          };
          return f;
      }
      function main() i32 {
          return 0;
      }
    )"),
                                "Borrow check failed");
}

TEST(Lambdas_RefCaptures, ref_conflict_while_captured) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [ref x] () void {
              x += 1;
          };
          ref r = x;
          return 0;
      }
    )"),
                                "Borrow check failed");
}

// ============================================================================
// Parser shape
// ============================================================================

TEST(Lambdas_RefCaptures, capture_list_names_on_proto) {
  std::istringstream ss("lambda [ref a, ref b] () void { a += b; }");
  Parser parser(ss);
  parser.getNextToken();
  auto expr = parser.parseExpression();

  ASSERT_NE(expr, nullptr);
  ASSERT_EQ(expr->getType(), ASTNodeType::LAMBDA);
  auto* lambda = static_cast<LambdaAST*>(expr.get());
  const auto& names = lambda->getProto().getRefCaptureNames();
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0], "a");
  EXPECT_EQ(names[1], "b");
}

// ============================================================================
// Owned captures: a capture list entry without `ref`
// ============================================================================

// A scalar has nothing to move, so [x] copies it, as an unlisted capture does
TEST(Lambdas_RefCaptures, owned_scalar_capture_copies) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [x] () i32 { return x + 41; };
          return f();
      }
    )");
  EXPECT_EQ(value, 42);
}

// The copy is the closure's own, so writing it leaves the original alone
TEST(Lambdas_RefCaptures, owned_scalar_capture_is_mutable_inside) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [x] () i32 { x = x + 1; return x; };
          var a = f();
          return a * 10 + x;
      }
    )");
  EXPECT_EQ(value, 21);
}

// A class moves into the closure, which owns it from then on
TEST(Lambdas_RefCaptures, owned_class_capture_moves) {
  auto value = executeString(R"(
      class Point {
          public var x: i32;
          function init(x: i32) { this.x = x; }
      }
      function main() i32 {
          var p = Point(42);
          var f = lambda [p] () i32 { return p.x; };
          return f();
      }
    )");
  EXPECT_EQ(value, 42);
}

// ...so the name it came from is gone. The borrow checker reports the detail
// on stderr and throws a summary, so this asserts on the throw.
TEST(Lambdas_RefCaptures, owned_class_capture_leaves_source_moved) {
  EXPECT_THROW(executeString(R"(
      class Point {
          public var x: i32;
          function init(x: i32) { this.x = x; }
      }
      function main() i32 {
          var p = Point(42);
          var f = lambda [p] () i32 { return p.x; };
          return p.x;
      }
    )"),
               SunError);
}

// The closure owns the value, so it drops it exactly once
TEST(Lambdas_RefCaptures, owned_class_capture_drops_once) {
  auto value = executeString(R"(
      var drops: i32 = 0;
      class Res {
          public var v: i32;
          function init(v: i32) { this.v = v; }
          function deinit() void { if (this.v != 0) { drops = drops + 1; } }
      }
      function main() i32 {
          if (true) {
              var r = Res(7);
              var f = lambda [r] () i32 { return r.v; };
              var used = f();
          }
          return drops;
      }
    )");
  EXPECT_EQ(value, 1);
}

// A borrow is somebody else's value; it cannot be laundered into an owned one
TEST(Lambdas_RefCaptures, owned_capture_of_a_ref_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Point {
          public var x: i32;
          function init(x: i32) { this.x = x; }
      }
      function read(p: ref Point) i32 {
          var f = lambda [p] () i32 { return p.x; };
          return f();
      }
      function main() i32 {
          var p = Point(1);
          return read(p);
      }
    )"),
                                "it is a reference");
}

TEST(Lambdas_RefCaptures, capture_named_twice_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [ref x, x] () i32 { return x; };
          return f();
      }
    )"),
                                "twice");
}

TEST(Lambdas_RefCaptures, owned_capture_unused_in_body_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var f = lambda [x] () i32 { return 1; };
          return f();
      }
    )"),
                                "does not use it");
}

// Moving a value into a lambda while it is borrowed would leave the borrow
// pointing at a husk
TEST(Lambdas_RefCaptures, owned_capture_while_borrowed_is_rejected) {
  EXPECT_THROW(executeString(R"(
      class Point {
          public var x: i32;
          function init(x: i32) { this.x = x; }
      }
      function main() i32 {
          var p = Point(1);
          var r: ref Point = p;
          var f = lambda [p] () i32 { return p.x; };
          return r.x;
      }
    )"),
               SunError);
}
