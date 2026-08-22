// tests/classes/test_struct_literals.cpp — `{ field: value }` construction
//
// A class that declares no `init` is built with a struct literal naming every
// field. Positional construction is deliberately not offered for these: field
// order is a layout detail, and a positional call would silently change
// meaning if two same-typed fields were ever reordered.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"
#include "parsing/formatter.h"

// ============================================================================
// Construction
// ============================================================================

TEST(Classes_StructLiterals, initializes_fields_by_name) {
  auto value = executeString(R"(
    class Car {
      var color: i32;
      var speed: i32;
    }
    function main() i32 {
      var car: Car = { color: 7, speed: 120 };
      return car.speed - car.color;
    }
  )");
  EXPECT_EQ(value, 113);
}

TEST(Classes_StructLiterals, field_order_does_not_have_to_match_declaration) {
  // Naming the fields is the point: the literal is order-independent.
  auto value = executeString(R"(
    class Car {
      var color: i32;
      var speed: i32;
    }
    function main() i32 {
      var car: Car = { speed: 120, color: 7 };
      return car.speed - car.color;
    }
  )");
  EXPECT_EQ(value, 113);
}

TEST(Classes_StructLiterals, mixed_field_types) {
  auto value = executeString(R"(
    class M {
      var n: i32;
      var big: i64;
      var flag: bool;
    }
    function main() i32 {
      var m: M = { n: 5, big: 100, flag: true };
      if (m.flag) { return m.n + m.big; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 105);
}

TEST(Classes_StructLiterals, trailing_comma_allowed) {
  auto value = executeString(R"(
    class P { var a: i32; var b: i32; }
    function main() i32 {
      var p: P = { a: 40, b: 2, };
      return p.a + p.b;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Classes_StructLiterals, single_field_class) {
  auto value = executeString(R"(
    class One { var v: i32; }
    function main() i32 {
      var o: One = { v: 42 };
      return o.v;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Classes_StructLiterals, field_values_can_be_expressions) {
  auto value = executeString(R"(
    class P { var a: i32; var b: i32; }
    function twice(x: i32) i32 { return x * 2; }
    function main() i32 {
      var n = 20;
      var p: P = { a: twice(n), b: n / 10 };
      return p.a + p.b;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Classes_StructLiterals, works_for_global_variables) {
  auto value = executeString(R"(
    class Point { var x: i32; var y: i32; }
    var g: Point = { x: 1, y: 2 };
    function main() i32 { return g.x + g.y; }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Classes_StructLiterals, nested_class_field) {
  auto value = executeString(R"(
    class Inner { var a: i32; var b: i32; }
    class Outer { var inner: Inner; var tag: i32; }
    function main() i32 {
      var o: Outer = { inner: { a: 10, b: 20 }, tag: 12 };
      return o.inner.a + o.inner.b + o.tag;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Diagnostics
// ============================================================================

TEST(Classes_StructLiterals, rejects_positional_construction) {
  // The whole point of the syntax: a class with no init cannot be built
  // positionally.
  EXPECT_THROW(executeString(R"(
    class Car { var color: i32; var speed: i32; }
    function main() i32 { var c = Car(7, 120); return c.speed; }
  )"),
               std::exception);
}

TEST(Classes_StructLiterals, rejects_unknown_field) {
  EXPECT_THROW(executeString(R"(
    class Car { var color: i32; var speed: i32; }
    function main() i32 {
      var c: Car = { color: 1, speeed: 2 };
      return 0;
    }
  )"),
               std::exception);
}

TEST(Classes_StructLiterals, rejects_missing_field) {
  // A field left out would silently be zero, which is the bug class this
  // syntax exists to prevent.
  EXPECT_THROW(executeString(R"(
    class Car { var color: i32; var speed: i32; }
    function main() i32 {
      var c: Car = { color: 1 };
      return 0;
    }
  )"),
               std::exception);
}

TEST(Classes_StructLiterals, rejects_duplicate_field) {
  EXPECT_THROW(executeString(R"(
    class Car { var color: i32; var speed: i32; }
    function main() i32 {
      var c: Car = { color: 1, color: 2, speed: 3 };
      return 0;
    }
  )"),
               std::exception);
}

TEST(Classes_StructLiterals, rejects_wrong_field_type) {
  EXPECT_THROW(executeString(R"(
    class Car { var color: static_ptr<u8>; var speed: i32; }
    function main() i32 {
      var c: Car = { color: 5, speed: 1 };
      return 0;
    }
  )"),
               std::exception);
}

TEST(Classes_StructLiterals, requires_a_type_annotation) {
  // A literal has no type of its own; without an annotation there is nothing
  // to check the field names against.
  EXPECT_THROW(executeString(R"(
    class Car { var color: i32; var speed: i32; }
    function main() i32 {
      var c = { color: 1, speed: 2 };
      return 0;
    }
  )"),
               std::exception);
}

TEST(Classes_StructLiterals, rejects_literal_for_a_class_with_init) {
  // Two ways to build one object would mean two sets of invariants.
  EXPECT_THROW(executeString(R"(
    class P {
      var a: i32;
      function init(a: i32) { this.a = a; }
    }
    function main() i32 {
      var p: P = { a: 1 };
      return 0;
    }
  )"),
               std::exception);
}

// ============================================================================
// Formatting
// ============================================================================

TEST(Classes_StructLiterals, formatter_round_trip) {
  EXPECT_EQ(sun::formatSource("class C{var a: i32;var b: i32;}\n"
                              "function main() i32{var c: C={a:1,b:2};"
                              "return c.a;}"),
            "class C {\n"
            "  var a: i32;\n"
            "  var b: i32;\n"
            "}\n"
            "function main() i32 {\n"
            "  var c: C = { a: 1, b: 2 };\n"
            "  return c.a;\n"
            "}\n");
}

TEST(Classes_StructLiterals, nested_class_field_in_a_global) {
  // The global initializer path is separate from the local one, and a
  // class-typed field must copy the nested struct rather than store its
  // address over the field's bytes.
  auto value = executeString(R"(
    class Inner { var a: i32; var b: i32; }
    class Outer { var inner: Inner; var tag: i32; }
    var o: Outer = { tag: 12, inner: { a: 10, b: 20 } };
    function main() i32 { return o.inner.a + o.inner.b + o.tag; }
  )");
  EXPECT_EQ(value, 42);
}
