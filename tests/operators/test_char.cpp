// tests/operators/test_char.cpp - Tests for the char type and the two literal
// forms: 'a' (a Unicode scalar value) and b'a' (a byte).

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Values and comparisons
// ============================================================================

TEST(Operators_Char, literal_roundtrips_through_a_variable) {
  auto value = executeString(R"(
      function main() i32 {
          var c: char = 'a';
          return _convert<i32>(c);
      }
    )");
  EXPECT_EQ(value, 97);
}

TEST(Operators_Char, literal_type_is_inferred_as_char) {
  auto value = executeString(R"(
      function main() i32 {
          var c = 'z';          // char, not i32
          if (c == 'z') { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Char, equality_and_inequality) {
  auto value = executeString(R"(
      function main() i32 {
          var a: char = 'a';
          var b: char = 'b';
          if (a == a and a != b) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// Ordering follows the scalar value, so 'Z' (0x5A) sorts before 'a' (0x61)
// and every ASCII char sorts before any astral one.
TEST(Operators_Char, ordering_follows_scalar_value) {
  auto value = executeString(R"(
      function main() i32 {
          if ('a' < 'b' and 'Z' < 'a' and 'a' <= 'a' and 'b' > 'a') {
              if ('a' < '\u{1F600}' and '\u{E9}' < '\u{1F600}') { return 1; }
          }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Char, escapes_have_their_ascii_values) {
  auto value = executeString(R"(
      function main() i32 {
          if (_convert<i32>('\n') == 10 and _convert<i32>('\t') == 9) {
              if (_convert<i32>('\\') == 92 and _convert<i32>('\'') == 39) {
                  if (_convert<i32>('\0') == 0 and _convert<i32>('\x41') == 65) {
                      return 1;
                  }
              }
          }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Char, unicode_literals_carry_the_scalar_value) {
  auto value = executeString(R"(
      function main() i32 {
          var emoji: char = '\u{1F600}';
          if (emoji == '😀' and _convert<i32>('é') == 233) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Byte literals
// ============================================================================

TEST(Operators_Char, byte_literal_is_a_u8) {
  auto value = executeString(R"(
      function main() i32 {
          var b: u8 = b'a';
          return _convert<i32>(b);
      }
    )");
  EXPECT_EQ(value, 97);
}

TEST(Operators_Char, byte_literal_reaches_the_whole_byte_range) {
  auto value = executeString(R"(
      function main() i32 {
          var b: u8 = b'\xFF';
          return _convert<i32>(b);
      }
    )");
  EXPECT_EQ(value, 255);
}

// A byte literal is a u8, so it widens like any other u8 value.
TEST(Operators_Char, byte_literal_widens_to_i64) {
  auto value = executeString(R"(
      function main() i32 {
          var n: i64 = b'z';
          if (n == 122) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Char, byte_literal_compares_against_a_wider_integer) {
  auto value = executeString(R"(
      function main() i32 {
          var n: i64 = 97;
          if (n == b'a') { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Conversions
// ============================================================================

TEST(Operators_Char, converts_to_and_from_integers) {
  auto value = executeString(R"(
      function main() i32 {
          var b: u8 = 97;
          var c: char = _convert<char>(b);
          var back: i64 = _convert<i64>(c);
          if (c == 'a' and back == 97) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Char, astral_scalar_survives_the_round_trip) {
  auto value = executeString(R"(
      function main() i32 {
          var n: i64 = 128512;                    // U+1F600
          var c: char = _convert<char>(n);
          if (c == '😀' and _convert<i64>(c) == n) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// char in the rest of the language
// ============================================================================

TEST(Operators_Char, works_as_a_parameter_and_return_type) {
  auto value = executeString(R"(
      function next_letter(c: char) char {
          if (c == 'a') { return 'b'; }
          return c;
      }
      function main() i32 {
          if (next_letter('a') == 'b') { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Char, works_as_a_match_scrutinee) {
  auto value = executeString(R"(
      function main() i32 {
          var c: char = 'b';
          return match c {
              'a' => 1,
              'b' => 2,
              _ => 0
          };
      }
    )");
  EXPECT_EQ(value, 2);
}

TEST(Operators_Char, works_as_an_array_element) {
  auto value = executeString(R"(
      function main() i32 {
          var letters: array<char, 3> = ['a', 'b', 'c'];
          if (letters[1] == 'b') { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Operators_Char, works_as_a_class_field) {
  auto value = executeString(R"(
      class Token {
          var sigil: char;
          init(sigil: char) { this.sigil = sigil; }
          public const function sigil_is(c: char) bool { return this.sigil == c; }
      }
      function main() i32 {
          var t = Token('$');
          if (t.sigil_is('$')) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Rejections
// ============================================================================

// char is a scalar value, not a small number: it never joins arithmetic.
TEST(Operators_Char, arithmetic_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        function main() i32 {
            var c: char = 'a';
            var n = c + 1;
            return 0;
        }
      )"),
                                "is not defined for 'char'");
}

TEST(Operators_Char, comparing_with_an_integer_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        function main() i32 {
            var c: char = 'a';
            if (c == 65) { return 1; }
            return 0;
        }
      )"),
                                "Cannot compare 'char' with 'i32'");
}

// The motivating mistake: String.at() hands back a byte, so it must be
// compared against a byte literal rather than a char.
TEST(Operators_Char, comparing_a_byte_with_a_char_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        function main() i32 {
            var b: u8 = 97;
            if (b == 'a') { return 1; }
            return 0;
        }
      )"),
                                "Cannot compare 'char' with 'u8'");
}

TEST(Operators_Char, an_integer_literal_is_not_a_char) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 {
            var c: char = 65;
            return 0;
        }
      )"),
      "Cannot assign value of type 'i32' to variable 'c' of type 'char'");
}

TEST(Operators_Char, a_char_does_not_assign_to_an_integer) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 {
            var c: char = 'a';
            var n: i32 = c;
            return 0;
        }
      )"),
      "Cannot assign value of type 'char' to variable 'n' of type 'i32'");
}

TEST(Operators_Char, negation_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 {
            var c: char = 'a';
            var d = -c;
            return 0;
        }
      )"),
      "Unary minus requires a numeric operand, got 'char'");
}

TEST(Operators_Char, bitwise_not_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 {
            var c: char = 'a';
            var d = ~c;
            return 0;
        }
      )"),
      "Bitwise NOT (~) requires an integer operand, got 'char'");
}

TEST(Operators_Char, converting_to_a_float_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        function main() i32 {
            var c: char = 'a';
            var x = _convert<f64>(c);
            return 0;
        }
      )"),
      "char converts to and from the integer types only");
}
