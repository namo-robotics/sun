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
        return s.find_char(111);  // 'o' at index 4
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
        return s.find_char(120);  // 'x' not in string
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
