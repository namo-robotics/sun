// tests/test_bitwise.cpp - Tests for bitwise operations (&, |, ^, <<, >>)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Bitwise AND (&)
// ============================================================================

TEST(BitwiseOps, and_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 15 & 10;
      }
    )");
  EXPECT_EQ(value, 10);  // 15 & 10 = 10
}

TEST(BitwiseOps, and_i32_decimal) {
  auto value = executeString(R"(
      function main() i32 {
          return 15 & 10;
      }
    )");
  EXPECT_EQ(value, 10);
}

TEST(BitwiseOps, and_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 255;
          var y: i32 = 15;
          return x & y;
      }
    )");
  EXPECT_EQ(value, 15);
}

TEST(BitwiseOps, and_mask_byte) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 305419896;
          return x & 255;
      }
    )");
  EXPECT_EQ(value, 120);  // 0x12345678 & 0xFF = 0x78 = 120
}

TEST(BitwiseOps, and_zero) {
  auto value = executeString(R"(
      function main() i32 {
          return 12345 & 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(BitwiseOps, and_identity) {
  auto value = executeString(R"(
      function main() i32 {
          return 42 & -1;
      }
    )");
  EXPECT_EQ(value, 42);  // -1 is all 1s
}

// ============================================================================
// Bitwise OR (|)
// ============================================================================

TEST(BitwiseOps, or_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 12 | 3;
      }
    )");
  EXPECT_EQ(value, 15);  // 12 | 3 = 15
}

TEST(BitwiseOps, or_i32_decimal) {
  auto value = executeString(R"(
      function main() i32 {
          return 12 | 3;
      }
    )");
  EXPECT_EQ(value, 15);
}

TEST(BitwiseOps, or_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 240;
          var y: i32 = 15;
          return x | y;
      }
    )");
  EXPECT_EQ(value, 255);
}

TEST(BitwiseOps, or_zero) {
  auto value = executeString(R"(
      function main() i32 {
          return 42 | 0;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(BitwiseOps, or_set_bits) {
  auto value = executeString(R"(
      function main() i32 {
          var flags: i32 = 0;
          flags = flags | 1;
          flags = flags | 4;
          return flags;
      }
    )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// Bitwise XOR (^)
// ============================================================================

TEST(BitwiseOps, xor_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 12 ^ 10;
      }
    )");
  EXPECT_EQ(value, 6);  // 12 ^ 10 = 6
}

TEST(BitwiseOps, xor_i32_decimal) {
  auto value = executeString(R"(
      function main() i32 {
          return 12 ^ 10;
      }
    )");
  EXPECT_EQ(value, 6);
}

TEST(BitwiseOps, xor_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 255;
          var y: i32 = 170;
          return x ^ y;
      }
    )");
  EXPECT_EQ(value, 85);
}

TEST(BitwiseOps, xor_self_is_zero) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 12345;
          return x ^ x;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(BitwiseOps, xor_zero) {
  auto value = executeString(R"(
      function main() i32 {
          return 42 ^ 0;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(BitwiseOps, xor_swap) {
  auto value = executeString(R"(
      function main() i32 {
          var a: i32 = 10;
          var b: i32 = 20;
          a = a ^ b;
          b = a ^ b;
          a = a ^ b;
          return a * 100 + b;
      }
    )");
  EXPECT_EQ(value, 2010);  // a=20, b=10
}

// ============================================================================
// Left Shift (<<)
// ============================================================================

TEST(BitwiseOps, lshift_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 1 << 4;
      }
    )");
  EXPECT_EQ(value, 16);
}

TEST(BitwiseOps, lshift_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 3;
          var y: i32 = 4;
          return x << y;
      }
    )");
  EXPECT_EQ(value, 48);
}

TEST(BitwiseOps, lshift_multiply_by_power_of_2) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 5;
          return x << 3;
      }
    )");
  EXPECT_EQ(value, 40);  // 5 * 8 = 40
}

TEST(BitwiseOps, lshift_zero) {
  auto value = executeString(R"(
      function main() i32 {
          return 42 << 0;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(BitwiseOps, lshift_overflow) {
  auto value = executeString(R"(
      function main() i32 {
          return 1 << 31;
      }
    )");
  EXPECT_EQ(value, static_cast<int32_t>(1u << 31));  // -2147483648
}

// ============================================================================
// Right Shift (>>)
// ============================================================================

TEST(BitwiseOps, rshift_i32) {
  auto value = executeString(R"(
      function main() i32 {
          return 16 >> 4;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(BitwiseOps, rshift_i32_variables) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 48;
          var y: i32 = 4;
          return x >> y;
      }
    )");
  EXPECT_EQ(value, 3);
}

TEST(BitwiseOps, rshift_divide_by_power_of_2) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 40;
          return x >> 3;
      }
    )");
  EXPECT_EQ(value, 5);  // 40 / 8 = 5
}

TEST(BitwiseOps, rshift_zero) {
  auto value = executeString(R"(
      function main() i32 {
          return 42 >> 0;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(BitwiseOps, rshift_negative_arithmetic) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = -16;
          return x >> 2;
      }
    )");
  EXPECT_EQ(value, -4);  // Arithmetic shift preserves sign
}

TEST(BitwiseOps, rshift_large_value) {
  auto value = executeString(R"(
      function main() i32 {
          return 255 >> 4;
      }
    )");
  EXPECT_EQ(value, 15);
}

// ============================================================================
// Operator Precedence
// ============================================================================

TEST(BitwiseOps, precedence_shift_before_bitwise) {
  auto value = executeString(R"(
      function main() i32 {
          return 1 << 2 | 1;
      }
    )");
  EXPECT_EQ(value, 5);  // (1 << 2) | 1 = 4 | 1 = 5
}

TEST(BitwiseOps, precedence_arithmetic_before_shift) {
  auto value = executeString(R"(
      function main() i32 {
          return 1 + 1 << 2;
      }
    )");
  EXPECT_EQ(value, 8);  // (1 + 1) << 2 = 2 << 2 = 8
}

TEST(BitwiseOps, precedence_and_before_or) {
  auto value = executeString(R"(
      function main() i32 {
          return 1 | 2 & 3;
      }
    )");
  EXPECT_EQ(value, 3);  // 1 | (2 & 3) = 1 | 2 = 3
}

TEST(BitwiseOps, precedence_and_before_xor) {
  auto value = executeString(R"(
      function main() i32 {
          return 1 ^ 3 & 2;
      }
    )");
  EXPECT_EQ(value, 3);  // 1 ^ (3 & 2) = 1 ^ 2 = 3
}

TEST(BitwiseOps, precedence_xor_before_or) {
  auto value = executeString(R"(
      function main() i32 {
          return 1 | 2 ^ 3;
      }
    )");
  EXPECT_EQ(value, 1);  // 1 | (2 ^ 3) = 1 | 1 = 1
}

// ============================================================================
// Complex Bitwise Expressions
// ============================================================================

TEST(BitwiseOps, bit_manipulation) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 0;
          x = x | (1 << 0);
          x = x | (1 << 2);
          x = x | (1 << 4);
          return x;
      }
    )");
  EXPECT_EQ(value, 21);  // bits 0, 2, 4 set
}

TEST(BitwiseOps, clear_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 255;
          var mask: i32 = 1 << 3;
          return x & (mask ^ -1);
      }
    )");
  EXPECT_EQ(value, 247);  // 255 with bit 3 cleared
}

TEST(BitwiseOps, toggle_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 10;
          x = x ^ (1 << 1);
          return x;
      }
    )");
  EXPECT_EQ(value, 8);  // Toggle bit 1: 10 -> 8
}

TEST(BitwiseOps, extract_nibble) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 305419896;
          return (x >> 8) & 15;
      }
    )");
  EXPECT_EQ(value, 6);  // Extract second nibble from low byte (0x12345678 >> 8) & 0xF = 0x56 & 0xF = 6
}

TEST(BitwiseOps, all_bits_set) {
  auto value = executeString(R"(
      function main() i32 {
          return -1;
      }
    )");
  EXPECT_EQ(value, -1);  // All bits set
}

TEST(BitwiseOps, power_of_two_check) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 16;
          var result: i32 = x & (x - 1);
          return result;
      }
    )");
  EXPECT_EQ(value, 0);  // 16 is power of 2
}

TEST(BitwiseOps, not_power_of_two) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 15;
          var result: i32 = x & (x - 1);
          return result;
      }
    )");
  EXPECT_EQ(value, 14);  // 15 is not power of 2
}

TEST(BitwiseOps, count_set_bits_simple) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 7;
          var count: i32 = 0;
          if ((x & 1) != 0) { count = count + 1; }
          if ((x & 2) != 0) { count = count + 1; }
          if ((x & 4) != 0) { count = count + 1; }
          if ((x & 8) != 0) { count = count + 1; }
          return count;
      }
    )");
  EXPECT_EQ(value, 3);  // 7 = 0b111 has 3 bits set
}

TEST(BitwiseOps, byte_pack_unpack) {
  auto value = executeString(R"(
      function main() i32 {
          var a: i32 = 1;
          var b: i32 = 2;
          var c: i32 = 3;
          var d: i32 = 4;
          var packed: i32 = (a << 24) | (b << 16) | (c << 8) | d;
          var unpacked_d: i32 = packed & 255;
          return unpacked_d;
      }
    )");
  EXPECT_EQ(value, 4);
}
