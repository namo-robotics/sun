// tests/test_unsigned.cpp - Tests for unsigned integer semantics
// (division, modulo, right shift, comparisons, widening)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Unsigned division / modulo
// ============================================================================

TEST(UnsignedOps, div_u32_high_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u32 = 3000000000;
          var y: u32 = x / 2;
          if (y == 1500000000) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(UnsignedOps, mod_u32_high_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u32 = 3000000000;
          var y: u32 = x % 7;
          if (y == 4) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);  // 3000000000 % 7 == 4 (signed srem would give 0)
}

TEST(UnsignedOps, div_u8_high_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u8 = 200;
          var y: u8 = x / 3;
          if (y == 66) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Unsigned right shift (logical, not arithmetic)
// ============================================================================

TEST(UnsignedOps, shr_u32_high_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u32 = 3000000000;
          var y: u32 = x >> 1;
          if (y == 1500000000) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(UnsignedOps, shr_u8_high_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u8 = 200;
          var y: u8 = x >> 2;
          if (y == 50) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Unsigned comparisons
// ============================================================================

TEST(UnsignedOps, compare_u8_high_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u8 = 200;
          var y: u8 = 100;
          if (x > y) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);  // signed compare would see 200 as -56
}

TEST(UnsignedOps, compare_u32_high_bit) {
  auto value = executeString(R"(
      function main() i32 {
          var x: u32 = 3000000000;
          var y: u32 = 5;
          var count: i32 = 0;
          if (x > y) { count = count + 1; }
          if (x >= y) { count = count + 1; }
          if (y < x) { count = count + 1; }
          if (y <= x) { count = count + 1; }
          return count;
      }
    )");
  EXPECT_EQ(value, 4);
}

// ============================================================================
// Unsigned widening (zext, not sext)
// ============================================================================

TEST(UnsignedOps, widen_u8_to_u32_binary) {
  auto value = executeString(R"(
      function main() i32 {
          var small: u8 = 200;
          var big: u32 = 1000;
          var sum: u32 = big + small;
          if (sum == 1200) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);  // sext of u8 200 would add -56
}

TEST(UnsignedOps, widen_u8_var_init) {
  auto value = executeString(R"(
      function main() i32 {
          var small: u8 = 200;
          var big: u32 = small;
          if (big == 200) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(UnsignedOps, widen_u8_call_arg) {
  auto value = executeString(R"(
      function take(v: u32) i32 {
          if (v == 200) { return 1; }
          return 0;
      }
      function main() i32 {
          var small: u8 = 200;
          return take(small);
      }
    )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Signed regressions (must keep signed semantics)
// ============================================================================

TEST(UnsignedOps, signed_div_regression) {
  auto value = executeString(R"(
      function main() i32 {
          return -7 / 2;
      }
    )");
  EXPECT_EQ(value, -3);
}

TEST(UnsignedOps, signed_compare_regression) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = -1;
          if (x > 1) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(UnsignedOps, signed_shr_regression) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = -8;
          return x >> 1;
      }
    )");
  EXPECT_EQ(value, -4);  // arithmetic shift preserves sign
}
