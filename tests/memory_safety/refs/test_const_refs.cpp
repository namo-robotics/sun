// tests/memory_safety/refs/test_const_refs.cpp - Constant references:
// `const ref r = x;`, `var r: const ref T = x;` and `const ref T` parameters
// read through a borrow and never write through it.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Reading through a const ref
// ============================================================================

TEST(MemorySafety_Refs_ConstRefs, statement_form_reads_target) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 41;
          const ref r = x;
          x = x + 1;
          return r;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Refs_ConstRefs, annotated_form_reads_field) {
  auto value = executeString(R"(
      class Point { var x: i32; function init(v: i32) { this.x = v; } }
      function main() i32 {
          var p = Point(7);
          var r: const ref i32 = p.x;
          return r * 6;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Refs_ConstRefs, parameter_reads_class) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          var y: i32;
          function init(a: i32, b: i32) { this.x = a; this.y = b; }
          const function sum() i32 { return this.x + this.y; }
      }
      function total(p: const ref Point) i32 { return p.sum() + p.x; }
      function main() i32 {
          var p = Point(3, 4);
          return total(p);
      }
    )");
  EXPECT_EQ(value, 10);
}

TEST(MemorySafety_Refs_ConstRefs, generic_const_ref_parameter) {
  auto value = executeString(R"(
      class Box<T> {
          var v: T;
          function init(v: T) { this.v = v; }
          const function get() T { return this.v; }
      }
      function first<T>(b: const ref Box<T>) T { return b.get(); }
      function main() i32 {
          var b = Box<i32>(42);
          return first<i32>(b);
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Refs_ConstRefs, mutable_ref_passes_to_const_ref) {
  auto value = executeString(R"(
      function peek(x: const ref i32) i32 { return x; }
      function bump(x: ref i32) i32 {
          x = x + 1;
          return peek(x);
      }
      function main() i32 {
          var v: i32 = 1;
          ref r = v;
          const ref c = r;
          return bump(v) + c;
      }
    )");
  EXPECT_EQ(value, 4);
}

// ============================================================================
// Writing through a const ref is rejected
// ============================================================================

TEST(MemorySafety_Refs_ConstRefs,
     assignment_through_statement_ref_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          const ref r = x;
          r = 2;
          return x;
      }
    )"),
                                "Cannot assign through const reference 'r'");
}

TEST(MemorySafety_Refs_ConstRefs, assignment_through_parameter_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function set(x: const ref i32) void { x = 2; }
      function main() i32 {
          var x: i32 = 1;
          set(x);
          return x;
      }
    )"),
                                "Cannot assign through const reference 'x'");
}

TEST(MemorySafety_Refs_ConstRefs, field_write_through_parameter_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Point { var x: i32; function init() { this.x = 0; } }
      function set(p: const ref Point) void { p.x = 2; }
      function main() i32 {
          var p = Point();
          set(p);
          return p.x;
      }
    )"),
      "Cannot assign to field 'x' of const reference 'p'");
}

TEST(MemorySafety_Refs_ConstRefs,
     non_const_method_through_parameter_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Counter {
          var n: i32;
          function init() { this.n = 0; }
          function bump() void { this.n = this.n + 1; }
      }
      function poke(c: const ref Counter) void { c.bump(); }
      function main() i32 {
          var c = Counter();
          poke(c);
          return c.n;
      }
    )"),
      "Cannot call non-const method 'bump' on const reference 'c'");
}

// ============================================================================
// const ref never becomes ref
// ============================================================================

TEST(MemorySafety_Refs_ConstRefs, rebinding_as_mutable_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var r: const ref i32 = x;
          ref m = r;
          m = 2;
          return x;
      }
    )"),
      "Cannot take a mutable reference to const reference 'r'");
}

TEST(MemorySafety_Refs_ConstRefs, const_ref_argument_to_ref_param_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      function bump(x: ref i32) void { x = x + 1; }
      function relay(x: const ref i32) void { bump(x); }
      function main() i32 {
          var x: i32 = 1;
          relay(x);
          return x;
      }
    )"),
      "No matching overload of 'bump' for argument types (const ref i32)");
}

TEST(MemorySafety_Refs_ConstRefs, downgrade_of_rebinding_is_allowed) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 20;
          ref m = x;
          const ref c = m;
          m = m + 1;
          return c;
      }
    )");
  EXPECT_EQ(value, 21);
}

// ============================================================================
// A `ref T` result seen through a constant receiver is `const ref T`
// ============================================================================

TEST(MemorySafety_Refs_ConstRefs, ref_result_reads_through_const_receiver) {
  auto value = executeString(R"(
      class Cell {
          var v: i32;
          function init(v: i32) { this.v = v; }
          const function get() ref i32 { return this.v; }
      }
      function main() i32 {
          const c = Cell(21);
          var r: const ref i32 = c.get();
          return r + c.get();
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Refs_ConstRefs, ref_result_cannot_bind_mutable_ref) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Cell {
          var v: i32;
          function init(v: i32) { this.v = v; }
          const function get() ref i32 { return this.v; }
      }
      function main() i32 {
          const c = Cell(21);
          var r: ref i32 = c.get();
          r = 1;
          return c.v;
      }
    )"),
                                "Cannot assign value of type 'const ref i32'");
}

TEST(MemorySafety_Refs_ConstRefs, ref_result_cannot_be_written_through) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Inner { var x: i32; function init() { this.x = 0; } }
      class Cell {
          var inner: Inner;
          function init() { this.inner = Inner(); }
          const function get() ref Inner { return this.inner; }
      }
      function main() i32 {
          const c = Cell();
          c.get().x = 1;
          return c.inner.x;
      }
    )"),
      "Cannot assign to field 'x' of a const reference");
}

TEST(MemorySafety_Refs_ConstRefs, ref_result_stays_mutable_on_var_receiver) {
  auto value = executeString(R"(
      class Cell {
          var v: i32;
          function init(v: i32) { this.v = v; }
          const function get() ref i32 { return this.v; }
      }
      function main() i32 {
          var c = Cell(1);
          var r: ref i32 = c.get();
          r = 42;
          return c.v;
      }
    )");
  EXPECT_EQ(value, 42);
}
