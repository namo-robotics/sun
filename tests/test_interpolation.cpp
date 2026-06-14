// tests/test_interpolation.cpp - Tests for string interpolation

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Interpolation Tests
// ============================================================================

TEST(InterpolationTest, simple_literal) {
  // Template string with no interpolation should work like a string literal
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = `Hello World`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(InterpolationTest, interpolate_string) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var name = String(allocator, "World");
        var s = `Hello ${name}!`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 12);  // "Hello World!"
}

TEST(InterpolationTest, interpolate_integer) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var age: i64 = 42;
        var s = `Age: ${age}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 7);  // "Age: 42"
}

TEST(InterpolationTest, interpolate_boolean_true) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var flag: bool = true;
        var s = `Flag: ${flag}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 10);  // "Flag: true"
}

TEST(InterpolationTest, interpolate_boolean_false) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var flag: bool = false;
        var s = `Flag: ${flag}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 11);  // "Flag: false"
}

TEST(InterpolationTest, multiple_interpolations) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var name = String(allocator, "Alice");
        var age: i64 = 30;
        var s = `${name} is ${age} years old`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 21);  // "Alice is 30 years old"
}

TEST(InterpolationTest, adjacent_interpolations) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var a: i64 = 1;
        var b: i64 = 2;
        var s = `${a}${b}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 2);  // "12"
}

TEST(InterpolationTest, interpolate_expression) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var x: i64 = 10;
        var y: i64 = 5;
        var s = `Sum: ${x + y}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 7);  // "Sum: 15"
}

TEST(InterpolationTest, interpolate_literal_string) {
  // String literal inside interpolation
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = `Hello ${"World"}!`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 12);  // "Hello World!"
}

TEST(InterpolationTest, escape_backtick) {
  // Escaped backtick inside template string
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = `Hello \` World`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 13);  // "Hello ` World"
}

TEST(InterpolationTest, escape_dollar) {
  // Escaped dollar sign (to prevent interpolation)
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = `Price: \${100}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 13);  // "Price: ${100}"
}

TEST(InterpolationTest, escape_newline) {
  // Newline escape sequence
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = `Line1\nLine2`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 11);  // "Line1\nLine2" - 11 chars including newline
}

TEST(InterpolationTest, empty_template) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = ``;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(InterpolationTest, only_interpolation) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var name = String(allocator, "Test");
        var s = `${name}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 4);  // "Test"
}

TEST(InterpolationTest, nested_braces_in_expression) {
  // Expression with braces (e.g., array literal)
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var arr: array<i64, 3> = [1, 2, 3];
        var s = `First: ${arr[0]}`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 8);  // "First: 1"
}

TEST(InterpolationTest, fails_without_stdlib) {
  // String interpolation requires stdlib for sun::String
  EXPECT_THROW(executeString(R"(
    function main() i64 {
        var x = 1;
        var s = `Value: ${x}`;
        return s.length();
    }
  )"),
               SunError);
}

TEST(InterpolationTest, works_without_using_sun) {
  // String interpolation should work with just stdlib, no 'using sun;' needed
  auto value = executeStringWithStdlib(R"(
    function main() i64 {
        var s = `Hello`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 5);
}
