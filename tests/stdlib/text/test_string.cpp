// tests/stdlib/text/test_string.cpp - Tests for String class

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic String Construction Tests
// ============================================================================

TEST(Stdlib_Text_String, construct_from_literal) {
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

TEST(Stdlib_Text_String, construct_empty) {
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

TEST(Stdlib_Text_String, construct_from_buffer) {
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

TEST(Stdlib_Text_String, at_access) {
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

TEST(Stdlib_Text_String, set_at_modify) {
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

TEST(Stdlib_Text_String, equals_literal_true) {
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

TEST(Stdlib_Text_String, equals_literal_false_content) {
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

TEST(Stdlib_Text_String, equals_literal_false_length) {
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

TEST(Stdlib_Text_String, append_char) {
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

TEST(Stdlib_Text_String, append_char_content) {
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

TEST(Stdlib_Text_String, append_literal) {
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
TEST(Stdlib_Text_String, append_string) {
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

TEST(Stdlib_Text_String, slice_basic) {
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

TEST(Stdlib_Text_String, slice_middle) {
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

TEST(Stdlib_Text_String, slice_content) {
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

TEST(Stdlib_Text_String, slice_equals_literal) {
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

TEST(Stdlib_Text_String, initial_capacity) {
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
TEST(Stdlib_Text_String, clear) {
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
TEST(Stdlib_Text_String, append_i64_positive) {
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
TEST(Stdlib_Text_String, append_i64_negative) {
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
TEST(Stdlib_Text_String, append_i64_zero) {
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
TEST(Stdlib_Text_String, append_hex) {
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
TEST(Stdlib_Text_String, append_bool_true) {
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
TEST(Stdlib_Text_String, append_bool_false) {
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
TEST(Stdlib_Text_String, find_char) {
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
TEST(Stdlib_Text_String, find_char_not_found) {
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
TEST(Stdlib_Text_String, starts_with) {
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
TEST(Stdlib_Text_String, ends_with) {
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
TEST(Stdlib_Text_String, reverse) {
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
TEST(Stdlib_Text_String, growth_beyond_initial_capacity) {
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

TEST(Stdlib_Text_String, literal_escape_newline_tab) {
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

TEST(Stdlib_Text_String, literal_escape_crlf) {
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

TEST(Stdlib_Text_String, literal_escape_quote_and_backslash) {
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

TEST(Stdlib_Text_String, literal_unknown_escape_preserved) {
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

TEST(Stdlib_Text_String, append_f64_shortest_round_trip) {
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

TEST(Stdlib_Text_String, parse_i64_and_parse_f64) {
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

// ============================================================================
// Case, Trim, Search, Split, Replace
// ============================================================================

TEST(Stdlib_Text_String, to_lower_and_to_upper) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var s = String(alloc, "Hello, World 42!");
        s.to_lower();
        if (not s.equals_literal("hello, world 42!")) { return 1; }
        s.to_upper();
        if (not s.equals_literal("HELLO, WORLD 42!")) { return 2; }
        // Case conversion of a single byte keeps the u8 type
        var ch: u8 = 65;
        var lowered: u8 = ch + 32;
        var one = String(alloc, "");
        one.append_char(lowered);
        if (not one.equals_literal("a")) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, trim_variants) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var s = String(alloc, "  \t hi \n ");
        s.trim();
        if (not s.equals_literal("hi")) { return 1; }

        var lead = String(alloc, "  hi  ");
        lead.trim_start();
        if (not lead.equals_literal("hi  ")) { return 2; }

        var trail = String(alloc, "  hi  ");
        trail.trim_end();
        if (not trail.equals_literal("  hi")) { return 3; }

        var blank = String(alloc, "   ");
        blank.trim();
        if (not blank.isEmpty()) { return 4; }

        var none = String(alloc, "hi");
        none.trim();
        if (not none.equals_literal("hi")) { return 5; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, find_and_contains) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function index_of(s: ref String, needle: static_ptr<u8>) i64 {
        return match s.find(needle) {
            Option.Some(i) => i,
            Option.None => -1
        };
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var s = String(alloc, "hello world");
        if (index_of(s, "hello") != 0) { return 1; }
        if (index_of(s, "world") != 6) { return 2; }
        if (index_of(s, "o w") != 4) { return 3; }
        if (index_of(s, "worlds") != -1) { return 4; }
        if (index_of(s, "") != 0) { return 5; }
        if (not s.contains("lo wo")) { return 6; }
        if (s.contains("xyz")) { return 7; }

        var needle = String(alloc, "wor");
        if (not s.contains(needle)) { return 8; }
        var missing = String(alloc, "cat");
        if (s.contains(missing)) { return 9; }
        if (not s.starts_with(String(alloc, "hell"))) { return 10; }
        if (not s.ends_with(String(alloc, "rld"))) { return 11; }
        if (s.starts_with(String(alloc, "world"))) { return 12; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, substr_and_clone) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32, IError {
        var alloc = make_heap_allocator();
        var s = String(alloc, "abcdef");
        var mid = s.substr(alloc, 2, 3);
        if (not mid.equals_literal("cde")) { return 1; }
        var empty = s.substr(alloc, 6, 0);
        if (not empty.isEmpty()) { return 2; }

        var copy = s.clone(alloc);
        if (not copy.equals(s)) { return 3; }
        copy.to_upper();
        if (not s.equals_literal("abcdef")) { return 4; }

        try {
            var bad = s.substr(alloc, 4, 3);
            return 5;
        } catch (e: IError) {
            return 0;
        }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, split_on_byte_and_string) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var csv = String(alloc, "a,b,,c");
        var parts = csv.split(alloc, 44);  // ','
        if (parts.size() != 4) { return 1; }
        if (not parts.get_unchecked(0).equals_literal("a")) { return 2; }
        if (not parts.get_unchecked(2).isEmpty()) { return 3; }
        if (not parts.get_unchecked(3).equals_literal("c")) { return 4; }

        var plain = String(alloc, "no separators here");
        var whole = plain.split(alloc, 44);
        if (whole.size() != 1) { return 5; }

        var text = String(alloc, "k1=v1;;k2=v2;");
        var fields = text.split(alloc, ";;");
        if (fields.size() != 2) { return 6; }
        if (not fields.get_unchecked(0).equals_literal("k1=v1")) { return 7; }
        if (not fields.get_unchecked(1).equals_literal("k2=v2;")) { return 8; }

        var untouched = String(alloc, "abc");
        var one = untouched.split(alloc, "");
        if (one.size() != 1) { return 9; }
        if (not one.get_unchecked(0).equals_literal("abc")) { return 10; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, split_nonempty_skips_empty_pieces) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var csv = String(alloc, ",a,,b,");
        var parts = csv.split_nonempty(alloc, 44);  // ','
        if (parts.size() != 2) { return 1; }
        if (not parts.get_unchecked(0).equals_literal("a")) { return 2; }
        if (not parts.get_unchecked(1).equals_literal("b")) { return 3; }

        var seps = String(alloc, ",,,");
        var none = seps.split_nonempty(alloc, 44);
        if (none.size() != 0) { return 4; }

        var text = String(alloc, ";;k1=v1;;;;k2=v2;;");
        var fields = text.split_nonempty(alloc, ";;");
        if (fields.size() != 2) { return 5; }
        if (not fields.get_unchecked(0).equals_literal("k1=v1")) { return 6; }
        if (not fields.get_unchecked(1).equals_literal("k2=v2")) { return 7; }

        var abc = String(alloc, "abc");
        var one = abc.split_nonempty(alloc, "");
        if (one.size() != 1) { return 8; }
        if (not one.get_unchecked(0).equals_literal("abc")) { return 9; }

        var blank = String(alloc, "");
        var nothing = blank.split_nonempty(alloc, "");
        if (nothing.size() != 0) { return 10; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, split_whitespace_collapses_runs) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var s = String(alloc, "  one\ttwo  \n three\r\n");
        var words = s.split_whitespace(alloc);
        if (words.size() != 3) { return 1; }
        if (not words.get_unchecked(0).equals_literal("one")) { return 2; }
        if (not words.get_unchecked(1).equals_literal("two")) { return 3; }
        if (not words.get_unchecked(2).equals_literal("three")) { return 4; }

        var solid = String(alloc, "word");
        var solo = solid.split_whitespace(alloc);
        if (solo.size() != 1) { return 5; }
        if (not solo.get_unchecked(0).equals_literal("word")) { return 6; }

        var spaces = String(alloc, " \t\n ");
        var empty = spaces.split_whitespace(alloc);
        if (empty.size() != 0) { return 7; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, join_parts) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var csv = String(alloc, "a,b,,c");
        var parts = csv.split(alloc, 44);
        var rejoined = join(alloc, parts, ",");
        if (not rejoined.equals(csv)) { return 1; }
        var dashed = join(alloc, parts, " - ");
        if (not dashed.equals_literal("a - b -  - c")) { return 2; }

        var single = Vec<String>(alloc, 2);
        single.push(String(alloc, "solo"));
        var alone = join(alloc, single, ",");
        if (not alone.equals_literal("solo")) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, replace_literal_and_string) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        // Replacement longer than the match: buffer has to grow
        var s = String(alloc, "one fish two fish");
        s.replace("fish", "lizard");
        if (not s.equals_literal("one lizard two lizard")) { return 1; }

        // Replacement shorter than the match
        s.replace("lizard", "ox");
        if (not s.equals_literal("one ox two ox")) { return 2; }

        // Same length
        s.replace("ox", "oy");
        if (not s.equals_literal("one oy two oy")) { return 3; }

        // Replacement containing the needle must not loop forever
        var loopy = String(alloc, "aaa");
        loopy.replace("a", "aa");
        if (not loopy.equals_literal("aaaaaa")) { return 4; }

        // Deleting matches
        var del = String(alloc, "x-y-z");
        del.replace("-", "");
        if (not del.equals_literal("xyz")) { return 5; }

        // No match leaves the string alone
        var same = String(alloc, "hello");
        same.replace("zz", "yy");
        if (not same.equals_literal("hello")) { return 6; }

        // Empty needle is a no-op
        same.replace("", "yy");
        if (not same.equals_literal("hello")) { return 7; }

        // String overloads
        var text = String(alloc, "cat dog cat");
        var from = String(alloc, "cat");
        var to = String(alloc, "bird");
        text.replace(from, to);
        if (not text.equals_literal("bird dog bird")) { return 8; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// C string interop
// ============================================================================

TEST(Stdlib_Text_String, c_str_terminates_without_changing_the_string) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var s = String(alloc, "abc");
        s.append_literal("def");
        var p = s.c_str();
        // A NUL sits just past the last byte, and the bytes are unchanged.
        if (unsafe { _load<u8>(p, 6); } != 0) { return 1; }
        if (unsafe { _load<u8>(p, 0); } != 97) { return 2; }
        if (s.length() != 6) { return 3; }
        if (not s.equals_literal("abcdef")) { return 4; }
        // Still usable, and re-terminating after a mutation still works.
        s.append_char(33);
        if (unsafe { _load<u8>(s.c_str(), 7); } != 0) { return 5; }
        if (s.length() != 7) { return 6; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, from_c_str_copies_the_bytes) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var src = String(alloc, "round trip");
        var copy = from_c_str(alloc, src.c_str());
        if (copy.length() != 10) { return 1; }
        if (not copy.equals_literal("round trip")) { return 2; }
        // The copy is independent of the source.
        src.clear();
        if (copy.length() != 10) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_String, from_c_str_handles_empty_and_null) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var alloc = make_heap_allocator();
        var empty = String(alloc, "");
        var a = from_c_str(alloc, empty.c_str());
        if (not a.isEmpty()) { return 1; }
        var b = from_c_str(alloc, null);
        if (not b.isEmpty()) { return 2; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
