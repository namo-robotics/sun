// tests/stdlib/test_bigint.cpp - The bit intrinsics BigUint builds on
// (_mul_hi_u64, _ctlz_u64, _cttz_u64). The BigUint behavior tests live in
// stdlib/bigint_tests.sun.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Stdlib_BigInt_BitIntrinsics, mul_hi_ctlz_cttz) {
  auto value = executeString(R"(
    function main() i32 {
        var one: u64 = 1;
        var all_ones: u64 = 0 - one;
        var top: u64 = one << 63;
        var two32: u64 = one << 32;
        // (2^64-1)^2 >> 64 == 2^64-2
        if (_mul_hi_u64(all_ones, all_ones) != all_ones - 1) { return 1; }
        if (_mul_hi_u64(top, 4) != 2) { return 2; }
        if (_mul_hi_u64(two32 * 4, two32) != 4) { return 3; }
        if (_mul_hi_u64(3, 4) != 0) { return 4; }
        if (_ctlz_u64(1) != 63) { return 5; }
        if (_ctlz_u64(top) != 0) { return 6; }
        if (_ctlz_u64(0) != 64) { return 7; }
        if (_cttz_u64(8) != 3) { return 8; }
        if (_cttz_u64(top) != 63) { return 9; }
        if (_cttz_u64(0) != 64) { return 10; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
