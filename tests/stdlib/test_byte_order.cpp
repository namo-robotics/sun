// tests/stdlib/test_byte_order.cpp - The _bswap_u16 / _bswap_u32 / _bswap_u64
// intrinsics byte_order.sun is built on. The helper tests live in
// stdlib/byte_order_tests.sun.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Stdlib_ByteOrder_Intrinsics, bswap_u16_u32_u64) {
  auto value = executeString(R"(
    function main() i32 {
        // 0x1234 -> 0x3412
        var a: u16 = 4660;
        if (_bswap_u16(a) != 13330) { return 1; }
        if (_bswap_u16(_bswap_u16(a)) != a) { return 2; }
        // 0xFF00 -> 0x00FF
        var b: u16 = 65280;
        if (_bswap_u16(b) != 255) { return 3; }

        // 0x12345678 -> 0x78563412
        var c: u32 = 305419896;
        if (_bswap_u32(c) != 2018915346) { return 4; }
        if (_bswap_u32(_bswap_u32(c)) != c) { return 5; }
        if (_bswap_u32(0) != 0) { return 6; }

        // 0x0123456789ABCDEF, checked by reversing it twice
        var d: u64 = 81985529216486895;
        if (_bswap_u64(_bswap_u64(d)) != d) { return 7; }
        // The lowest byte becomes the highest, and back again
        var one: u64 = 1;
        if (_bswap_u64(one) != one << 56) { return 8; }
        var high: u64 = 255;
        high = high << 56;
        if (_bswap_u64(high) != 255) { return 9; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
