// tests/test_ref_targets.cpp - Tests for references to fields and array
// elements (ref r = obj.field; / ref r = arr[i];)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Ref to class field
// ============================================================================

TEST(RefTargets, ref_to_field_read) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          function init() void {
              this.x = 42;
          }
      }
      function main() i32 {
          var p: Point = Point();
          ref r = p.x;
          return r;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(RefTargets, ref_to_field_write_through) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          function init() void {
              this.x = 1;
          }
      }
      function main() i32 {
          var p: Point = Point();
          ref r = p.x;
          r = 9;
          return p.x;
      }
    )");
  EXPECT_EQ(value, 9);
}

// ============================================================================
// Ref to array element
// ============================================================================

TEST(RefTargets, ref_to_element_read) {
  auto value = executeString(R"(
      function main() i32 {
          var arr: array<i32, 3> = [10, 20, 30];
          ref r = arr[1];
          return r;
      }
    )");
  EXPECT_EQ(value, 20);
}

TEST(RefTargets, ref_to_element_write_through) {
  auto value = executeString(R"(
      function main() i32 {
          var arr: array<i32, 3> = [10, 20, 30];
          ref r = arr[2];
          r = 99;
          return arr[2];
      }
    )");
  EXPECT_EQ(value, 99);
}

TEST(RefTargets, ref_to_element_compound_write) {
  auto value = executeString(R"(
      function main() i32 {
          var arr: array<i32, 3> = [10, 20, 30];
          ref r = arr[0];
          r += 5;
          return arr[0];
      }
    )");
  EXPECT_EQ(value, 15);
}

// ============================================================================
// Rejected target shapes
// ============================================================================

TEST(RefTargets, class_index_target_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Box {
          var slot: i32;
          function init() void {
              this.slot = 1;
          }
          function __index__(indices: ref array<i64>) i32 {
              return this.slot;
          }
      }
      function main() i32 {
          var b: Box = Box();
          ref r = b[0];
          return 0;
      }
    )"),
                                "no storage address");
}

TEST(RefTargets, expression_target_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 1;
          ref r = x + 1;
          return 0;
      }
    )"),
                                "Reference target must be");
}

// ============================================================================
// Ref arguments: true addresses instead of spilled copies
// ============================================================================

TEST(RefTargets, ref_arg_field_mutation_visible) {
  auto value = executeString(R"(
      class Point {
          var x: i32;
          function init() void {
              this.x = 1;
          }
      }
      function bump(v: ref i32) void {
          v = v + 10;
      }
      function main() i32 {
          var p: Point = Point();
          bump(p.x);
          return p.x;
      }
    )");
  EXPECT_EQ(value, 11);  // previously mutated a spilled copy
}

TEST(RefTargets, ref_arg_element_mutation_visible) {
  auto value = executeString(R"(
      function bump(v: ref i32) void {
          v = v + 10;
      }
      function main() i32 {
          var arr: array<i32, 3> = [1, 2, 3];
          bump(arr[1]);
          return arr[1];
      }
    )");
  EXPECT_EQ(value, 12);  // previously an error: no IndexAST ref-arg support
}

TEST(RefTargets, ref_arg_local_still_works) {
  auto value = executeString(R"(
      function bump(v: ref i32) void {
          v = v + 10;
      }
      function main() i32 {
          var x: i32 = 5;
          bump(x);
          return x;
      }
    )");
  EXPECT_EQ(value, 15);
}
