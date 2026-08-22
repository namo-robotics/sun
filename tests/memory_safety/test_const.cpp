// tests/memory_safety/test_const.cpp - Constant variables: `const x = ...`
// can be read, borrowed with `const ref`, and moved as a whole, but never
// assigned, mutably borrowed, or taken apart.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Reading and borrowing
// ============================================================================

TEST(MemorySafety_Const, scalar_is_readable) {
  auto value = executeString(R"(
      function main() i32 {
          const x: i32 = 40;
          const y = x + 2;
          return y;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Const, class_fields_are_readable) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          var y: i32;
          function init(px: i32, py: i32) { this.x = px; this.y = py; }
      }
      function main() i32 {
          const p = Point(3, 4);
          return p.x * 10 + p.y;
      }
    )");
  EXPECT_EQ(value, 34);
}

TEST(MemorySafety_Const, const_ref_borrow_and_by_value_copy) {
  auto value = executeString(R"(
      function twice(x: i32) i32 { return x * 2; }
      function peek(x: const ref i32) i32 { return x + 1; }
      function main() i32 {
          const c: i32 = 10;
          const ref r = c;
          var s: const ref i32 = c;
          return twice(c) + peek(c) + r + s;
      }
    )");
  EXPECT_EQ(value, 20 + 11 + 10 + 10);
}

TEST(MemorySafety_Const, global_const_is_readable) {
  auto value = executeString(R"(
      const LIMIT: i32 = 7;
      function main() i32 {
          return LIMIT * 6;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Const, for_in_const_binding_reads_elements) {
  auto value = executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          v.push(1);
          v.push(2);
          v.push(3);
          var sum: i32 = 0;
          for (const x: i32 in v) {
              sum = sum + x;
          }
          return sum;
      }
    )");
  EXPECT_EQ(value, 6);
}

// ============================================================================
// Writes are rejected
// ============================================================================

TEST(MemorySafety_Const, assignment_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          const x: i32 = 1;
          x = 2;
          return x;
      }
    )"),
                                "Cannot assign to constant 'x'");
}

TEST(MemorySafety_Const, compound_assignment_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          const x: i32 = 1;
          x += 2;
          return x;
      }
    )"),
                                "Cannot assign to constant 'x'");
}

TEST(MemorySafety_Const, field_assignment_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Point {
          var x: i32;
          function init() { this.x = 0; }
      }
      function main() i32 {
          const p = Point();
          p.x = 5;
          return p.x;
      }
    )"),
                                "Cannot assign to field 'x' of constant 'p'");
}

TEST(MemorySafety_Const, nested_field_assignment_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Inner { var v: i32; function init() { this.v = 0; } }
      class Outer { var inner: Inner; function init() { this.inner = Inner(); } }
      function main() i32 {
          const o = Outer();
          o.inner.v = 5;
          return o.inner.v;
      }
    )"),
                                "Cannot assign to field 'v' of constant 'o'");
}

TEST(MemorySafety_Const, element_assignment_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          const a: array<i32, 3> = [1, 2, 3];
          a[0] = 9;
          return a[0];
      }
    )"),
                                "Cannot assign to an element of constant 'a'");
}

TEST(MemorySafety_Const, global_assignment_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      const LIMIT: i32 = 7;
      function main() i32 {
          LIMIT = 8;
          return LIMIT;
      }
    )"),
                                "Cannot assign to constant 'LIMIT'");
}

TEST(MemorySafety_Const, for_in_const_binding_is_not_assignable) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          v.push(1);
          for (const x: i32 in v) {
              x = 2;
          }
          return 0;
      }
    )"),
                                "Cannot assign to constant 'x'");
}

// ============================================================================
// Mutable borrows are rejected, const borrows are fine
// ============================================================================

TEST(MemorySafety_Const, ref_statement_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          const x: i32 = 1;
          ref r = x;
          return r;
      }
    )"),
                                "Cannot take a mutable reference to constant 'x'");
}

TEST(MemorySafety_Const, annotated_ref_binding_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          const x: i32 = 1;
          var r: ref i32 = x;
          return r;
      }
    )"),
                                "Cannot take a mutable reference to constant 'x'");
}

TEST(MemorySafety_Const, ref_argument_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function bump(x: ref i32) void { x = x + 1; }
      function main() i32 {
          const x: i32 = 1;
          bump(x);
          return x;
      }
    )"),
                                "Cannot pass as 'ref' argument 1 of 'bump' constant 'x'");
}

TEST(MemorySafety_Const, ref_argument_of_field_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Point { var x: i32; function init() { this.x = 0; } }
      function bump(x: ref i32) void { x = x + 1; }
      function main() i32 {
          const p = Point();
          bump(p.x);
          return p.x;
      }
    )"),
                                "Cannot pass as 'ref' argument 1 of 'bump' constant 'p'");
}

TEST(MemorySafety_Const, const_ref_of_const_is_allowed) {
  auto value = executeString(R"(
      class Point { var x: i32; function init(v: i32) { this.x = v; } }
      function read(p: const ref Point) i32 { return p.x; }
      function main() i32 {
          const p = Point(21);
          const ref q = p;
          return read(p) + q.x;
      }
    )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Moves
// ============================================================================

TEST(MemorySafety_Const, whole_value_move_is_allowed) {
  auto value = executeString(R"(
      class Point { var x: i32; function init(v: i32) { this.x = v; } }
      function take(p: Point) i32 { return p.x; }
      function main() i32 {
          const a = Point(1);
          var b = a;
          const c = Point(2);
          return b.x + take(c);
      }
    )");
  EXPECT_EQ(value, 3);
}

TEST(MemorySafety_Const, returning_const_local_is_allowed) {
  auto value = executeString(R"(
      class Point { var x: i32; function init(v: i32) { this.x = v; } }
      function make() Point {
          const p = Point(5);
          return p;
      }
      function main() i32 {
          var p = make();
          return p.x;
      }
    )");
  EXPECT_EQ(value, 5);
}

TEST(MemorySafety_Const, partial_move_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Inner { var v: i32; function init() { this.v = 1; } }
      class Outer { var inner: Inner; function init() { this.inner = Inner(); } }
      function main() i32 {
          const o = Outer();
          var i = o.inner;
          return i.v;
      }
    )"),
                                "Cannot move field 'inner' out of constant 'o'");
}

TEST(MemorySafety_Const, partial_move_by_argument_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Inner { var v: i32; function init() { this.v = 1; } }
      class Outer { var inner: Inner; function init() { this.inner = Inner(); } }
      function take(i: Inner) i32 { return i.v; }
      function main() i32 {
          const o = Outer();
          return take(o.inner);
      }
    )"),
                                "Cannot move field 'inner' out of constant 'o'");
}

TEST(MemorySafety_Const, moving_const_global_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Point { var x: i32; function init(v: i32) { this.x = v; } }
      const ORIGIN: Point = Point(0);
      function main() i32 {
          var p = ORIGIN;
          return p.x;
      }
    )"),
                                "Cannot move constant global 'ORIGIN'");
}

// ============================================================================
// Lambdas
// ============================================================================

TEST(MemorySafety_Const, by_ref_capture_can_read_but_not_write) {
  auto value = executeString(R"(
      function main() i32 {
          const c: i32 = 20;
          var get = lambda [ref c] () i32 { return c + 1; };
          return get() + c;
      }
    )");
  EXPECT_EQ(value, 41);

  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          const c: i32 = 20;
          var set = lambda [ref c] () void { c = 1; };
          set();
          return c;
      }
    )"),
                                "Cannot assign to constant 'c'");
}

// ============================================================================
// Borrow checker: `const ref` is a shared loan
// ============================================================================

TEST(MemorySafety_Const, two_const_refs_may_coexist) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 5;
          const ref a = x;
          const ref b = x;
          return a + b;
      }
    )");
  EXPECT_EQ(value, 10);
}

TEST(MemorySafety_Const, mutable_ref_while_const_ref_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 5;
          const ref a = x;
          ref b = x;
          b = 6;
          return a;
      }
    )"),
                                "Borrow check failed");
}

// ============================================================================
// Stdlib: a constant String is still useful
// ============================================================================

TEST(MemorySafety_Const, const_string_reads_and_prints) {
  auto value = executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          const s = String(allocator, "hello");
          println(s);
          const t = s.clone(allocator);
          if (not s.equals(t)) { return -1; }
          return _convert<i32>(s.length()) + _convert<i32>(s.at(0));
      }
    )");
  EXPECT_EQ(value, 5 + 'h');
}

TEST(MemorySafety_Const, const_string_cannot_be_appended) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          const s = String(allocator, "hello");
          s.append("!");
          return 0;
      }
    )"),
                                "Cannot call non-const method 'append' on constant 's'");
}
