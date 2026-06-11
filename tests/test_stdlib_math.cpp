// tests/test_stdlib_math.cpp - Tests for stdlib generic math functions

#include <gtest/gtest.h>

#include <string>

#include "execution_utils.h"

// ============================================================================
// min<T>
// ============================================================================

TEST(StdlibMathTest, min_i32) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return min<i32>(10, 5);
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(StdlibMathTest, min_i32_reversed) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return min<i32>(3, 9);
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(StdlibMathTest, min_f64) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var result = min<f64>(3.5, 2.1);
        if (result < 2.2) {
            if (result > 2.0) {
                return 1;
            }
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// max<T>
// ============================================================================

TEST(StdlibMathTest, max_i32) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return max<i32>(10, 5);
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(StdlibMathTest, max_i64) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        return max<i64>(100, 200);
    }
  )");
  EXPECT_EQ(value, 200);
}

// ============================================================================
// abs<T>
// ============================================================================

TEST(StdlibMathTest, abs_positive) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return abs<i32>(42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(StdlibMathTest, abs_negative) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return abs<i32>(-42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(StdlibMathTest, abs_zero) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return abs<i32>(0);
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// clamp<T>
// ============================================================================

TEST(StdlibMathTest, clamp_within_range) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return clamp<i32>(5, 0, 10);
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(StdlibMathTest, clamp_below_min) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return clamp<i32>(-5, 0, 10);
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StdlibMathTest, clamp_above_max) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return clamp<i32>(15, 0, 10);
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// sign<T>
// ============================================================================

TEST(StdlibMathTest, sign_positive) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return sign<i32>(42);
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(StdlibMathTest, sign_negative) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return sign<i32>(-42);
    }
  )");
  EXPECT_EQ(value, -1);
}

TEST(StdlibMathTest, sign_zero) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        return sign<i32>(0);
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// in_range<T>
// ============================================================================

TEST(StdlibMathTest, in_range_true) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        if (in_range<i32>(5, 0, 10)) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(StdlibMathTest, in_range_at_boundary) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var count: i32 = 0;
        if (in_range<i32>(0, 0, 10)) { count = count + 1; }
        if (in_range<i32>(10, 0, 10)) { count = count + 1; }
        return count;
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(StdlibMathTest, in_range_false) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        if (in_range<i32>(15, 0, 10)) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Combined usage
// ============================================================================

TEST(StdlibMathTest, combined_min_max) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var a: i32 = 5;
        var b: i32 = 10;
        var c: i32 = 3;
        // min of max pairs
        var m1 = max<i32>(a, b);  // 10
        var m2 = max<i32>(b, c);  // 10
        return min<i32>(m1, m2);  // 10
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(StdlibMathTest, clamp_uses_minmax_pattern) {
  // Verify clamp behaves like max(lo, min(x, hi))
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var x: i32 = 15;
        var lo: i32 = 0;
        var hi: i32 = 10;
        var clamped = clamp<i32>(x, lo, hi);
        var manual = max<i32>(lo, min<i32>(x, hi));
        if (clamped == manual) {
            return clamped;
        }
        return -1;
    }
  )");
  EXPECT_EQ(value, 10);
}
