// tests/stdlib/test_byte_order.cpp - stdlib/byte_order.sun and the _bswap_u16
// / _bswap_u32 / _bswap_u64 intrinsics it is built on

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

TEST(Stdlib_ByteOrder, swap_and_endian_helpers) {
  auto value = executeStringWithStdlib(R"(
    using std.byte_order;

    function main() i32 {
        var a: u16 = 4660;         // 0x1234
        var b: u32 = 305419896;    // 0x12345678
        var c: u64 = 81985529216486895;

        if (swap_bytes_u16(a) != 13330) { return 1; }
        if (swap_bytes_u32(b) != 2018915346) { return 2; }
        if (swap_bytes_u64(swap_bytes_u64(c)) != c) { return 3; }

        // Big-endian is the opposite of every machine Sun targets, so the
        // conversions reverse the bytes and undo each other.
        if (to_big_endian_u16(a) != 13330) { return 4; }
        if (to_big_endian_u32(b) != 2018915346) { return 5; }
        if (from_big_endian_u16(to_big_endian_u16(a)) != a) { return 6; }
        if (from_big_endian_u32(to_big_endian_u32(b)) != b) { return 7; }
        if (from_big_endian_u64(to_big_endian_u64(c)) != c) { return 8; }

        // Little-endian already matches the machine, so nothing moves.
        if (to_little_endian_u16(a) != a) { return 9; }
        if (to_little_endian_u32(b) != b) { return 10; }
        if (to_little_endian_u64(c) != c) { return 11; }
        if (from_little_endian_u16(a) != a) { return 12; }
        if (from_little_endian_u32(b) != b) { return 13; }
        if (from_little_endian_u64(c) != c) { return 14; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// A two-byte length and a four-byte value written big-endian, then read back
// out of the bytes, the way a binary wire format uses these helpers.
TEST(Stdlib_ByteOrder, round_trip_through_a_byte_buffer) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.byte_order;

    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var buf = Vec<u8>(alloc, 4);

        var length: u16 = 1024;
        var wire_length: u16 = to_big_endian_u16(length);
        buf.push(_convert<u8>(wire_length & 255));
        buf.push(_convert<u8>((wire_length >> 8) & 255));

        // 1024 big-endian is 0x04 0x00
        if (buf.get(0) != 4) { return 1; }
        if (buf.get(1) != 0) { return 2; }

        var low: u16 = _convert<u16>(buf.get(0));
        var high: u16 = _convert<u16>(buf.get(1));
        var read_back: u16 = from_big_endian_u16(low | (high << 8));
        if (read_back != length) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
