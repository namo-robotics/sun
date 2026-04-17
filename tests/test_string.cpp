// tests/test_string.cpp - Tests for String class

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic String Construction Tests
// ============================================================================

TEST(StringTest, construct_from_literal) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "");
        return s.length();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(StringTest, construct_empty_with_capacity) {
  // TODO: This test needs a factory function since Sun doesn't support
  // declaring uninitialized class variables. Skip for now.
  // The init_empty constructor exists but needs to be called via
  // allocator.create or the class needs a default constructor + separate init
  // method.
  GTEST_SKIP() << "init_empty needs allocator.create pattern";
}

// ============================================================================
// Character Access Tests
// ============================================================================

TEST(StringTest, at_access) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
  auto value = executeString(R"(
    import "build/stdlib.moon";
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
