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
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        function main() i32 { var b = 21u9; return 0; }
      )"),
                                "Invalid numeric literal suffix 'u9'");
}

TEST(Operators_Literals, float_suffix_on_integer_form_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        function main() i32 { var b = 2f32; return 0; }
      )"),
                                "write 2.0f32");
}

TEST(Operators_Literals, integer_suffix_on_float_form_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
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

TEST(Operators_Literals, untyped_literal_argument_adopts_parameter_type) {
  EXPECT_EQ(executeString(R"(
        /* Checks a contextually typed unsigned literal. */
        function classify(id: u8) i32 { if (id == 21u8) { return 1; } return 0; }
        /* Passes an unsuffixed literal to a plain function. */
        function main() i32 { return classify(21); }
      )"), 1);
}

TEST(Operators_Literals, typed_variable_still_never_narrows) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        function classify(id: u8) i32 { return 1; }
        function main() i32 {
            var a: i32 = 5;
            return classify(a);
        }
      )"),
                                "No matching overload of 'classify'");
}

// ============================================================================
// The full u64 range: literals above i64's maximum keep their value (#211)
// ============================================================================

TEST(Operators_Literals, u64_literal_above_i64_max_keeps_its_value) {
  auto value = executeString(R"(
      function main() i32 {
          var v: u64 = 18446744073709551615u64;
          const m: u64 = 14627333968358193854;
          if (v != 18446744073709551615) { return 1; }
          if (v / 2 != 9223372036854775807) { return 2; }
          if (m != 14627333968358193854u64) { return 3; }
          if (m > v) { return 4; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(Operators_Literals, untyped_literal_above_i64_max_defaults_to_u64) {
  auto value = executeString(R"(
      function takes_u64(x: u64) i32 { if (x == 10000000000000000000) { return 1; } return 0; }
      function main() i32 {
          var big = 10000000000000000000;
          return takes_u64(big);
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Literals, i64_min_as_suffixed_literal) {
  auto value = executeString(R"(
      function main() i32 {
          var lo = -9223372036854775808i64;
          if (lo < -9223372036854775807 and lo + 1 == -9223372036854775807) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Literals, literal_above_i64_max_never_fits_i64) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var x = 9223372036854775808i64; return 0; }
      )"),
      "Integer literal 9223372036854775808 cannot be represented as 'i64'");
}

TEST(Operators_Literals, negative_literal_past_i64_min_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var x = -9223372036854775809i64; return 0; }
      )"),
      "Integer literal -9223372036854775809 cannot be represented as 'i64'");
}

TEST(Operators_Literals, literal_above_u64_max_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var x: u64 = 18446744073709551616; return 0; }
      )"),
      "Integer literal 18446744073709551616 is too large");
}

TEST(Operators_Literals, suffixed_literal_above_u64_max_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 { var x = 99999999999999999999u64; return 0; }
      )"),
      "Integer literal 99999999999999999999 is too large");
}

// Unsuffixed negative arguments use the same range checks as positive literals.
TEST(Operators_Literals, negative_method_arguments_adopt_parameter_types) {
  auto value = executeString(R"(
      /* Checks signed arguments after parameter conversion. */
      class Enc {
          init() {}
          /* Checks a narrow signed argument. */
          public method put_i8(v: i8) bool { return v == -5i8; }
          /* Checks a wider signed argument. */
          public method put_i16(v: i16) bool { return v == -300i16; }
          /* Checks the smallest signed value. */
          public method min_i8(v: i8) bool { return v == -128i8; }
          /* Checks the smallest wider signed value. */
          public method min_i16(v: i16) bool { return v == -32768i16; }
          /* Checks that widening preserves the sign. */
          public method put_i64(v: i64) bool { return v == -5i64; }
      }
      /* Exercises contextual typing of method arguments. */
      function main() i32 {
          var e = Enc();
          if (e.put_i8(-5) and e.put_i16(-300) and e.min_i8(-128) and
              e.min_i16(-32768) and e.put_i64(-5) and e.put_i8(-5i8)) {
              return 1;
          }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Literals, negative_untyped_literals_preserve_wide_values) {
  auto value = executeString(R"(
      /* Checks signed literal initialization and assignment. */
      function main() i32 {
          var wide: i64 = -5;
          if (wide != -5i64) { return 1; }
          wide = -300;
          var minimum: i64 = -9223372036854775808;
          if (wide != -300i64 or minimum != -9223372036854775808i64) {
              return 2;
          }
          return 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(Operators_Literals, negative_method_argument_out_of_range_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      /* Accepts a narrow signed argument. */
      class Enc {
          init() {}
          /* Accepts a narrow signed argument. */
          public method put(v: i8) void {}
      }
      /* Passes an out-of-range literal. */
      function main() i32 { var e = Enc(); e.put(-129); return 0; }
    )"), "expected i8, got i32");
}

TEST(Operators_Literals, negative_method_argument_never_fits_unsigned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      /* Accepts an unsigned argument. */
      class Enc {
          init() {}
          /* Accepts an unsigned argument. */
          public method put(v: u8) void {}
      }
      /* Passes a negative literal to an unsigned parameter. */
      function main() i32 { var e = Enc(); e.put(-1); return 0; }
    )"), "expected u8, got i32");
}

TEST(Operators_Literals, negative_untyped_literal_past_i64_min_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      /* Rejects a negative integer outside the supported signed range. */
      function main() i32 { var x = -9223372036854775809; return 0; }
    )"), "Integer literal -9223372036854775809 cannot be represented");
}

TEST(Operators_Literals, negative_plain_function_arguments) {
  EXPECT_EQ(executeString(R"(
      /* Returns its narrow argument. */
      function take(x: i8) i8 { return x; }
      /* Returns its wider argument. */
      function take16(x: i16) i16 { return x; }
      /* Checks negative literals at plain function boundaries. */
      function main() i32 {
          if (take(-5) != -5i8 or take(-128) != -128i8 or
              take16(-300) != -300i16 or take16(-32768) != -32768i16) {
              return 1;
          }
          return 0;
      }
    )"), 0);
}

TEST(Operators_Literals, negative_plain_function_overload_keeps_default_type) {
  EXPECT_EQ(executeString(R"(
      /* Identifies the narrow overload. */
      function pick(x: i8) i32 { return 1; }
      /* Identifies the default integer overload. */
      function pick(x: i32) i32 { return 2; }
      /* Checks that contextual typing does not displace an existing match. */
      function main() i32 { return pick(-5) + pick(-5i8); }
    )"), 3);
}

TEST(Operators_Literals, negative_plain_function_literal_overloads_are_ambiguous) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      /* Accepts a narrow integer. */
      function take(x: i8) void {}
      /* Accepts a wider integer. */
      function take(x: i16) void {}
      /* Requires a suffix when both contextual types fit. */
      function main() i32 { take(-5); return 0; }
    )"), "Ambiguous overload of 'take'");
}

TEST(Operators_Literals, negative_plain_function_rejects_invalid_arguments) {
  for (const auto& argument : {"-129", "-5i16", "value"}) {
    EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(
        std::string(R"(
          /* Accepts a narrow integer. */
          function take(x: i8) void {}
          /* Rejects overflowing literals and narrowing typed values. */
          function main() i32 { var value: i32 = -5; take(
        )") + argument + "); return 0; }"), "No matching overload of 'take'");
  }
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      /* Accepts an unsigned integer. */
      function take(x: u8) void {}
      /* Rejects negative values for unsigned parameters. */
      function main() i32 { take(-5); return 0; }
    )"), "No matching overload of 'take'");
}

TEST(Operators_Literals, plain_function_context_checks_every_argument) {
  EXPECT_EQ(executeString(R"(
      /* Identifies a candidate whose second argument does not fit. */
      function pick(x: i8, y: bool) i32 { return 1; }
      /* Identifies the candidate that accepts both arguments. */
      function pick(x: i16, y: i32) i32 {
          if (x == -5i16 and y == 7) { return 2; }
          return 0;
      }
      /* Identifies the narrower range. */
      function range(x: i8) i32 { return 3; }
      /* Identifies the wider range. */
      function range(x: i16) i32 {
          if (x == -300i16) { return 4; }
          return 0;
      }
      /* Checks candidate isolation and literal range filtering. */
      function main() i32 { return pick(-5, 7) + range(-300); }
    )"), 6);
}
