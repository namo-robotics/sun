// tests/stdlib/text/test_interpolation.cpp - Tests for string interpolation

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Interpolation Tests
// ============================================================================

TEST(Stdlib_Text_Interpolation, simple_literal) {
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

TEST(Stdlib_Text_Interpolation, interpolate_string) {
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

TEST(Stdlib_Text_Interpolation, interpolate_integer) {
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

TEST(Stdlib_Text_Interpolation, interpolate_boolean_true) {
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

TEST(Stdlib_Text_Interpolation, interpolate_boolean_false) {
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

TEST(Stdlib_Text_Interpolation, multiple_interpolations) {
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

TEST(Stdlib_Text_Interpolation, adjacent_interpolations) {
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

TEST(Stdlib_Text_Interpolation, interpolate_expression) {
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

TEST(Stdlib_Text_Interpolation, interpolate_literal_string) {
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

TEST(Stdlib_Text_Interpolation, escape_backtick) {
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

TEST(Stdlib_Text_Interpolation, escape_dollar) {
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

TEST(Stdlib_Text_Interpolation, escape_newline) {
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

TEST(Stdlib_Text_Interpolation, empty_template) {
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

TEST(Stdlib_Text_Interpolation, only_interpolation) {
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

TEST(Stdlib_Text_Interpolation, nested_braces_in_expression) {
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

TEST(Stdlib_Text_Interpolation, fails_without_stdlib) {
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

TEST(Stdlib_Text_Interpolation, println_interpolated_direct) {
  // Interpolated string passed directly as a ref String argument
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var x: i32 = 42;
        println(`x = ${x}`);
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_Interpolation, print_interpolated_direct) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var x: i64 = 7;
        print(`a=${x}`);
        println();
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_Interpolation, println_interpolated_twice) {
  // Back-to-back interpolated temporaries stress repeated temp cleanup
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var a: i64 = 1;
        var b: i64 = 2;
        println(`a=${a}`);
        println(`b=${b}`);
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Text_Interpolation, works_without_using_sun) {
  // String interpolation should work with just stdlib, no 'using sun;' needed
  auto value = executeStringWithStdlib(R"(
    function main() i64 {
        var s = `Hello`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Stdlib_Text_Interpolation, interpolate_float) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var ratio: f64 = 0.25;
        var s = `ratio=${ratio} half=${1.5}`;
        if (s.equals_literal("ratio=0.25 half=1.5") == false) { return -1; }
        return s.length();
    }
  )");
  EXPECT_EQ(value, 19);
}
