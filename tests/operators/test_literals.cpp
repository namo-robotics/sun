// tests/operators/test_literals.cpp - Tests for suffixed numeric literals
// (21u8, -128i8, 1.5f32) and for the untyped-literal rules they leave intact

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Suffixed literals carry their own type into call arguments
// ============================================================================

TEST(Operators_Literals, suffixed_args_reach_narrow_parameters) {
  auto value = executeString(R"(
      function classify(id: u8) i32 { if (id == 21) { return 1; } return 0; }
      function takes_u16(x: u16) i32 { if (x == 7400) { return 1; } return 0; }
      function takes_u32(x: u32) i32 { if (x == 70000) { return 1; } return 0; }
      function takes_i8(x: i8) i32 { if (x == -5) { return 1; } return 0; }
      function main() i32 {
          return classify(21u8) + takes_u16(7400u16) + takes_u32(70000u32) +
                 takes_i8(-5i8);
      }
    )");
  EXPECT_EQ(value, 4);
}

TEST(Operators_Literals, suffixed_constructor_arguments) {
  auto value = executeString(R"(
      class Addr {
          var ip: u32;
          var port: u16;
          init(ip: u32, port: u16) {
              this.ip = ip;
              this.port = port;
          }
      }
      function main() i32 {
          var sa = Addr(0u32, 7400u16);
          if (sa.ip == 0 and sa.port == 7400) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Literals, suffixed_method_argument) {
  auto value = executeString(R"(
      class Box {
          var v: u16;
          init() { this.v = 0; }
          method set(v: u16) void { this.v = v; }
      }
      function main() i32 {
          var b = Box();
          b.set(7400u16);
          if (b.v == 7400) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Overload resolution sees the suffix type
// ============================================================================

TEST(Operators_Literals, suffix_picks_overload_and_untyped_stays_i32) {
  auto value = executeString(R"(
      function pick(x: u8) i32 { return 1; }
      function pick(x: i32) i32 { return 2; }
      function main() i32 {
          if (pick(21u8) == 1 and pick(21) == 2) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// A suffixed literal is a typed value: it widens but never narrows
// ============================================================================

TEST(Operators_Literals, suffixed_literal_widens_like_a_typed_value) {
  auto value = executeString(R"(
      function widen(x: u16) i32 { if (x == 21) { return 1; } return 0; }
      function main() i32 {
          var big: u64 = 21u8;
          if (widen(21u8) == 1 and big == 21) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Literals, suffixed_literal_never_narrows) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var x: u8 = 21u16; return 0; }
      )"),
      "Cannot assign value of type 'u16' to variable 'x' of type 'u8'");
}

TEST(Operators_Literals, suffixed_operand_keeps_its_type_in_arithmetic) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u16 = 100u16;
          var y = x + 1u16;
          var wrapped: u8 = 200u8 + 100u8;
          if (y == 101 and wrapped == 44) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Negative suffixed literals: the minus folds into the literal
// ============================================================================

TEST(Operators_Literals, negative_suffixed_literals_including_boundary) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i8 = -128i8;
          var y: i8 = -1i8;
          var z: i64 = -1i64;
          if (x == -128 and y == -1 and z == -1) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Float suffixes
// ============================================================================

TEST(Operators_Literals, float_suffixes_type_the_literal) {
  auto value = executeString(R"(
      function bump(x: f32) f32 { return x + 0.5f32; }
      function main() i32 {
          var f = bump(1.5f32);
          var d: f64 = 2.5f64;
          var e = 1e2f32;
          if (f == 2.0f32 and d == 2.5 and e == 100.0f32) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Out-of-range and malformed suffixes are compile errors
// ============================================================================

TEST(Operators_Literals, out_of_range_suffixed_value_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var b = 300u8; return 0; }
      )"),
      "Integer literal 300 cannot be represented as 'u8'");
}

TEST(Operators_Literals, negative_out_of_range_suffixed_value_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var x = -129i8; return 0; }
      )"),
      "Integer literal -129 cannot be represented as 'i8'");
}

TEST(Operators_Literals, negative_value_never_fits_unsigned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var x = -1u8; return 0; }
      )"),
      "Integer literal -1 cannot be represented as 'u8'");
}

TEST(Operators_Literals, unknown_suffix_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var b = 21u9; return 0; }
      )"),
      "Invalid numeric literal suffix 'u9'");
}

TEST(Operators_Literals, float_suffix_on_integer_form_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var b = 2f32; return 0; }
      )"),
      "write 2.0f32");
}

TEST(Operators_Literals, integer_suffix_on_float_form_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var b = 1.5u8; return 0; }
      )"),
      "Invalid suffix 'u8' on a float literal");
}

// ============================================================================
// Untyped literal rules are unchanged
// ============================================================================

TEST(Operators_Literals, untyped_literal_still_adapts_at_declaration) {
  auto value = executeString(R"(
      function main() i32 {
          var d: u8 = 255;
          if (d == 255) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Literals, untyped_literal_argument_still_stays_i32) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function classify(id: u8) i32 { return 1; }
        function main() i32 { return classify(21); }
      )"),
      "No matching overload of 'classify'");
}

TEST(Operators_Literals, typed_variable_still_never_narrows) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function classify(id: u8) i32 { return 1; }
        function main() i32 {
            var a: i32 = 5;
            return classify(a);
        }
      )"),
      "No matching overload of 'classify'");
}
