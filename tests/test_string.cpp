// tests/test_string.cpp - Tests for String class

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic String Construction Tests
// ============================================================================

TEST(StringTest, construct_from_literal) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        return s.length();
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(StringTest, construct_empty) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        return s.length();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StringTest, construct_from_buffer) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var buf = ContiguousBuffer<u8>(allocator, 10);
        // Write "Hi" (72, 105) to buffer
        buf.set_unchecked(0, 72);
        buf.set_unchecked(1, 105);
        
        try {
            var s = String(allocator, buf, 2);
            var len: i64 = s.length();
            if (len != 2) {
                return 100 + len;  // Debug: length wrong
            }
            var h: i64 = s.at(0);
            if (h != 72) {
                return 200 + h;  // Debug: first char wrong
            }
            var i: i64 = s.at(1);
            if (i != 105) {
                return 300 + i;  // Debug: second char wrong
            }
            return 1;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Character Access Tests
// ============================================================================

TEST(StringTest, at_access) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "ABCDE");
        // 'A' = 65, 'B' = 66, 'C' = 67, 'D' = 68, 'E' = 69
        var a: i64 = s.at(0);  // Convert u8 to i64
        var c: i64 = s.at(2);
        var e: i64 = s.at(4);
        return a * 10000 + c * 100 + e;
    }
  )");
  EXPECT_EQ(value, 65 * 10000 + 67 * 100 + 69);  // 656769
}

TEST(StringTest, set_at_modify) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        // Change 'H' (72) to 'J' (74)
        s.set_at(0, 74);
        var ch: i64 = s.at(0);
        return ch;
    }
  )");
  EXPECT_EQ(value, 74);  // 'J'
}

// ============================================================================
// String Comparison Tests
// ============================================================================

TEST(StringTest, equals_literal_true) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        if (s.equals_literal("Hello")) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(StringTest, equals_literal_false_content) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        if (s.equals_literal("World")) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StringTest, equals_literal_false_length) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        if (s.equals_literal("Hell")) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Append Tests
// ============================================================================

TEST(StringTest, append_char) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hi");
        s.append_char(33);  // '!'
        return s.length();
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(StringTest, append_char_content) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hi");
        s.append_char(33);  // '!'
        // Check last character is '!'
        var ch: i64 = s.at(2);
        return ch;
    }
  )");
  EXPECT_EQ(value, 33);  // '!'
}

TEST(StringTest, append_literal) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        s.append_literal(", World");
        return s.length();
    }
  )");
  EXPECT_EQ(value, 12);  // "Hello, Sun" = 12 chars
}
TEST(StringTest, append_string) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s1 = String(allocator, "Hello");
        var s2 = String(allocator, " World");
        s1.append(s2);
        return s1.length();
    }
  )");
  EXPECT_EQ(value, 11);  // "Hello World"
}

// ============================================================================
// Slicing Tests
// ============================================================================

TEST(StringTest, slice_basic) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello World");
        var view = s[0:5];  // "Hello"
        return view.length();
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(StringTest, slice_middle) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello World");
        var view = s[6:11];  // "World"
        return view.length();
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(StringTest, slice_content) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello World");
        var view = s[0:5];  // "Hello"
        // Check first and last char: 'H' (72), 'o' (111)
        var h: i64 = view.at(0);
        var o: i64 = view.at(4);
        return h * 1000 + o;
    }
  )");
  EXPECT_EQ(value, 72 * 1000 + 111);  // 72111
}

TEST(StringTest, slice_equals_literal) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello World");
        var view = s[6:11];  // "World"
        if (view.equals_literal("World")) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Capacity Tests
// ============================================================================

TEST(StringTest, initial_capacity) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        // Capacity should be length + 16 = 21
        if (s.capacity() >= s.length()) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test clear()
TEST(StringTest, clear) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        s.clear();
        return s.length();
    }
  )");
  EXPECT_EQ(value, 0);
}

// Test append_i64 with positive number
TEST(StringTest, append_i64_positive) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        s.append_i64(12345);
        if (s.equals_literal("12345")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test append_i64 with negative number
TEST(StringTest, append_i64_negative) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        s.append_i64(-42);
        if (s.equals_literal("-42")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test append_i64 with zero
TEST(StringTest, append_i64_zero) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        s.append_i64(0);
        if (s.equals_literal("0")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test append_hex
TEST(StringTest, append_hex) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        s.append_hex(255);
        if (s.equals_literal("ff")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test append_bool
TEST(StringTest, append_bool_true) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        s.append_bool(true);
        if (s.equals_literal("true")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test append_bool false
TEST(StringTest, append_bool_false) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        s.append_bool(false);
        if (s.equals_literal("false")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test find_char
TEST(StringTest, find_char) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "hello world");
        return match s.find_char(111) {   // 'o' at index 4
            Option.Some(i) => i,
            Option.None => -1
        };
    }
  )");
  EXPECT_EQ(value, 4);
}

// Test find_char not found
TEST(StringTest, find_char_not_found) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "hello");
        return match s.find_char(120) {   // 'x' not in string
            Option.Some(i) => i,
            Option.None => -1
        };
    }
  )");
  EXPECT_EQ(value, -1);
}

// Test starts_with
TEST(StringTest, starts_with) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "hello world");
        if (s.starts_with("hello")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test ends_with
TEST(StringTest, ends_with) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "hello world");
        if (s.ends_with("world")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test reverse
TEST(StringTest, reverse) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "abc");
        s.reverse();
        if (s.equals_literal("cba")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// Test dynamic growth beyond initial capacity
TEST(StringTest, growth_beyond_initial_capacity) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        for (var i: i64 = 0; i < 100; i = i + 1) {
            s.append_char(65);  // 'A'
        }
        return s.length();
    }
  )");
  EXPECT_EQ(value, 100);
}

// ============================================================================
// String Literal Escape Sequences
// ============================================================================

TEST(StringTest, literal_escape_newline_tab) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "a\nb\tc");
        if (s.length() != 5) { return 1; }
        if (s.at(1) != 10) { return 2; }
        if (s.at(3) != 9) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StringTest, literal_escape_crlf) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "\r\n");
        if (s.length() != 2) { return 1; }
        if (s.at(0) != 13) { return 2; }
        if (s.at(1) != 10) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StringTest, literal_escape_quote_and_backslash) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "say \"hi\" \\ done");
        // s = say "hi" \ done  (15 chars)
        if (s.length() != 15) { return 1; }
        if (s.at(4) != 34) { return 2; }   // '"'
        if (s.at(9) != 92) { return 3; }   // '\'
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StringTest, literal_unknown_escape_preserved) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "a\qb");
        // Unknown escape \q stays as backslash + q
        if (s.length() != 4) { return 1; }
        if (s.at(1) != 92) { return 2; }   // '\'
        if (s.at(2) != 113) { return 3; }  // 'q'
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Float Formatting and Number Parsing
// ============================================================================

TEST(StringTest, append_f64_shortest_round_trip) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function check(alloc: ref HeapAllocator, x: f64, expected: static_ptr<u8>) bool {
        var s = String(alloc, "");
        s.append_f64(x);
        return s.equals_literal(expected);
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        if (check(alloc, 0.1, "0.1") == false) { return 1; }
        if (check(alloc, 1.5, "1.5") == false) { return 2; }
        if (check(alloc, 100.0, "100") == false) { return 3; }
        if (check(alloc, 1e21, "1e+21") == false) { return 4; }
        if (check(alloc, -2.5e-7, "-2.5e-07") == false) { return 5; }
        if (check(alloc, 3.141592653589793, "3.141592653589793") == false) { return 6; }
        if (check(alloc, 1.0 / 3.0, "0.3333333333333333") == false) { return 7; }
        if (check(alloc, -0.0, "-0") == false) { return 8; }
        var f: f32 = 2.5;
        var s = String(alloc, "");
        s.append(f);
        s.append(0.25);
        if (s.equals_literal("2.50.25") == false) { return 9; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StringTest, parse_i64_and_parse_f64) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function int_or(alloc: ref HeapAllocator, text: static_ptr<u8>, fallback: i64) i64 {
        var s = String(alloc, text);
        return match s.parse_i64() {
            Option.Some(v) => v,
            Option.None => fallback
        };
    }

    function float_or(alloc: ref HeapAllocator, text: static_ptr<u8>, fallback: f64) f64 {
        var s = String(alloc, text);
        return match s.parse_f64() {
            Option.Some(v) => v,
            Option.None => fallback
        };
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        if (int_or(alloc, "42", -1) != 42) { return 1; }
        if (int_or(alloc, "-42", -1) != -42) { return 2; }
        if (int_or(alloc, "9223372036854775807", -1) != 9223372036854775807) { return 3; }
        if (int_or(alloc, "-9223372036854775808", -1) != -9223372036854775807 - 1) { return 4; }
        if (int_or(alloc, "9223372036854775808", -1) != -1) { return 5; }
        if (int_or(alloc, "12a", -1) != -1) { return 6; }
        if (int_or(alloc, "", -1) != -1) { return 7; }
        if (int_or(alloc, "-", -1) != -1) { return 8; }
        if (int_or(alloc, "1.5", -1) != -1) { return 9; }
        if (float_or(alloc, "1.5", 0.0) != 1.5) { return 10; }
        if (float_or(alloc, "-2.5e3", 0.0) != -2500.0) { return 11; }
        if (float_or(alloc, "7", 0.0) != 7.0) { return 12; }
        if (float_or(alloc, "0.1", 0.0) != 0.1) { return 13; }
        if (float_or(alloc, "abc", -1.0) != -1.0) { return 14; }
        if (float_or(alloc, "1e", -1.0) != -1.0) { return 15; }
        if (float_or(alloc, "1.5x", -1.0) != -1.0) { return 16; }
        if (float_or(alloc, ".", -1.0) != -1.0) { return 17; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
