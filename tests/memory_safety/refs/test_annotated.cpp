// tests/memory_safety/refs/test_annotated.cpp - Tests for borrows bound by an
// annotated declaration (var r: ref T = obj.field;) including conditional
// targets (var r: ref T = c ? a : b;)

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// var r: ref T = <lvalue>
// ============================================================================

TEST(MemorySafety_Refs_Annotated, binds_local) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 5;
          var r: ref i32 = x;
          r = 9;
          return x;
      }
    )");
  EXPECT_EQ(value, 9);
}

TEST(MemorySafety_Refs_Annotated, binds_field) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          var y: i32;
          function init() void {
              this.x = 1;
              this.y = 2;
          }
      }
      function main() i32 {
          var p: Point = Point();
          var r: ref i32 = p.x;
          r = 5;
          return p.x + p.y;
      }
    )");
  EXPECT_EQ(value, 7);
}

TEST(MemorySafety_Refs_Annotated, binds_element) {
  auto value = executeString(R"(
      function main() i32 {
          var arr: array<i32, 3> = [10, 20, 30];
          var r: ref i32 = arr[1];
          r = 5;
          return arr[1];
      }
    )");
  EXPECT_EQ(value, 5);
}

TEST(MemorySafety_Refs_Annotated, binds_class_without_moving_it) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          function init() void { this.x = 7; }
      }
      function main() i32 {
          var p: Point = Point();
          var r: ref Point = p;
          return r.x + p.x;
      }
    )");
  EXPECT_EQ(value, 14);
}

TEST(MemorySafety_Refs_Annotated, binds_field_inside_method) {
  auto value = executeString(R"(
      class Counter {
          var n: i32;
          function init() void { this.n = 3; }
          function bump() void {
              var c: ref i32 = this.n;
              c = c + 1;
          }
      }
      function main() i32 {
          var c: Counter = Counter();
          c.bump();
          return c.n;
      }
    )");
  EXPECT_EQ(value, 4);
}

// A class field never moves out of its object here - the borrow leaves the
// stored String intact, so the object still owns (and drops) it once.
TEST(MemorySafety_Refs_Annotated, binds_string_field) {
  auto value = executeStringWithStdlib(R"(
      using sun;
      public class Holder {
          public var a: String;
          public function init(alloc: ref HeapAllocator) {
              this.a = String(alloc, "one");
          }
      }
      function main() i64 {
          var alloc = make_heap_allocator();
          var h = Holder(alloc);
          var pick: ref String = h.a;
          return pick.length();
      }
    )");
  EXPECT_EQ(value, 3);
}

TEST(MemorySafety_Refs_Annotated, temporary_target_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var alloc = make_heap_allocator();
          var r: ref String = String(alloc, "temp");
          return 0;
      }
    )"),
                                "Cannot bind reference");
}

TEST(MemorySafety_Refs_Annotated, expression_target_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var r: ref i32 = x + 1;
          return 0;
      }
    )"),
                                "Cannot bind reference");
}

TEST(MemorySafety_Refs_Annotated, class_index_target_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Box {
          var slot: i32;
          function init() void { this.slot = 1; }
          function __index__(indices: ref array<i64>) i32 {
              return this.slot;
          }
      }
      function main() i32 {
          var b: Box = Box();
          var r: ref i32 = b[0];
          return 0;
      }
    )"),
                                "no storage address");
}

TEST(MemorySafety_Refs_Annotated, borrowing_a_moved_field_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
      using sun;
      public class Holder {
          public var a: String;
          public function init(alloc: ref HeapAllocator) {
              this.a = String(alloc, "one");
          }
      }
      function main() i32 {
          var alloc = make_heap_allocator();
          var h = Holder(alloc);
          var moved = h.a;
          var pick: ref String = h.a;
          return 0;
      }
    )"),
                                "Borrow check failed");
}

// ============================================================================
// Conditional targets: the borrow binds whichever branch runs
// ============================================================================

TEST(MemorySafety_Refs_Annotated, conditional_binds_chosen_field) {
  auto value = executeString(R"(
      class Pair {
          var a: i32;
          var b: i32;
          var flag: bool;
          function init() void {
              this.a = 1;
              this.b = 2;
              this.flag = false;
          }
      }
      function main() i32 {
          var p: Pair = Pair();
          var pick: ref i32 = p.flag ? p.a : p.b;
          return pick;
      }
    )");
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Refs_Annotated, conditional_writes_through_to_chosen_slot) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var y: i32 = 2;
          var takeFirst: bool = true;
          var r: ref i32 = takeFirst ? x : y;
          r = 90;
          return x + y;
      }
    )");
  EXPECT_EQ(value, 92);  // x is rebound to 90, y untouched
}

TEST(MemorySafety_Refs_Annotated, conditional_binds_distinct_classes) {
  auto value = executeStringWithStdlib(R"(
      using sun;
      function main() i64 {
          var alloc = make_heap_allocator();
          var s1 = String(alloc, "aaa");
          var s2 = String(alloc, "bb");
          var takeFirst: bool = false;
          var r: ref String = takeFirst ? s1 : s2;
          return r.length();
      }
    )");
  EXPECT_EQ(value, 2);
}

// The `ref r = ...` form accepts the same conditional target
TEST(MemorySafety_Refs_Annotated, conditional_ref_statement) {
  auto value = executeString(R"(
      class Pair {
          var a: i32;
          var b: i32;
          function init() void {
              this.a = 4;
              this.b = 8;
          }
      }
      function main() i32 {
          var p: Pair = Pair();
          var takeFirst: bool = true;
          ref r = takeFirst ? p.a : p.b;
          return r;
      }
    )");
  EXPECT_EQ(value, 4);
}

TEST(MemorySafety_Refs_Annotated, conditional_temporary_branch_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          var takeFirst: bool = true;
          var r: ref i32 = takeFirst ? x : x + 1;
          return 0;
      }
    )"),
                                "Cannot bind reference");
}
