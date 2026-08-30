// tests/stdlib/test_math.cpp - Tests for stdlib generic math functions

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// compute_min<T>
// ============================================================================

TEST(Stdlib_Math, min_i32) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_min<i32>(10, 5);
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Stdlib_Math, min_i32_reversed) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_min<i32>(3, 9);
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Stdlib_Math, min_f64) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var result = compute_min<f64>(3.5, 2.1);
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
// compute_max<T>
// ============================================================================

TEST(Stdlib_Math, max_i32) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_max<i32>(10, 5);
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(Stdlib_Math, max_i64) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        return compute_max<i64>(100, 200);
    }
  )");
  EXPECT_EQ(value, 200);
}

// ============================================================================
// compute_abs<T>
// ============================================================================

TEST(Stdlib_Math, abs_positive) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_abs<i32>(42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Math, abs_negative) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_abs<i32>(-42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Math, abs_zero) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_abs<i32>(0);
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// clamp<T>
// ============================================================================

TEST(Stdlib_Math, clamp_within_range) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return clamp<i32>(5, 0, 10);
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Stdlib_Math, clamp_below_min) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return clamp<i32>(-5, 0, 10);
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Math, clamp_above_max) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return clamp<i32>(15, 0, 10);
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// compute_sign<T>
// ============================================================================

TEST(Stdlib_Math, sign_positive) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_sign<i32>(42);
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Stdlib_Math, sign_negative) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_sign<i32>(-42);
    }
  )");
  EXPECT_EQ(value, -1);
}

TEST(Stdlib_Math, sign_zero) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        return compute_sign<i32>(0);
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// is_in_range<T>
// ============================================================================

TEST(Stdlib_Math, in_range_true) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        if (is_in_range<i32>(5, 0, 10)) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Stdlib_Math, in_range_at_boundary) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var count: i32 = 0;
        if (is_in_range<i32>(0, 0, 10)) { count = count + 1; }
        if (is_in_range<i32>(10, 0, 10)) { count = count + 1; }
        return count;
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Stdlib_Math, in_range_false) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        if (is_in_range<i32>(15, 0, 10)) {
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

TEST(Stdlib_Math, combined_min_max) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var a: i32 = 5;
        var b: i32 = 10;
        var c: i32 = 3;
        // compute_min of compute_max pairs
        var m1 = compute_max<i32>(a, b);  // 10
        var m2 = compute_max<i32>(b, c);  // 10
        return compute_min<i32>(m1, m2);  // 10
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(Stdlib_Math, clamp_uses_minmax_pattern) {
  // Verify clamp behaves like compute_max(lo, compute_min(x, hi))
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var x: i32 = 15;
        var lo: i32 = 0;
        var hi: i32 = 10;
        var clamped = clamp<i32>(x, lo, hi);
        var manual = compute_max<i32>(lo, compute_min<i32>(x, hi));
        if (clamped == manual) {
            return clamped;
        }
        return -1;
    }
  )");
  EXPECT_EQ(value, 10);
}
