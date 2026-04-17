// tests/test_generics.cpp - Tests for generic class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

TEST(GenericFunctions, generic_identity_function) {
  auto value = executeString(R"(
    function identity <T> (x: T) T {
        return x;
    }

    function main() i32 {
        return identity<i32>(42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, generic_function_with_two_type_params) {
  auto value = executeString(R"(
    function foo <T, U> (x: T, y: U) T {
        if (x == 42) {
            return x;
        } if (y == 3.14) {
            return -x;
        }
    }

    function main() i32 {
        return foo<i32, f64>(43, 3.14);
    }
  )");
  EXPECT_EQ(value, -43);
}

TEST(GenericFunctions, two_specializations) {
  auto value = executeString(R"(
    function identity <T> (x: T) T {
        return x;
    }

    function main() i64 {
        var a = identity<i32>(42);
        var b = identity<i64>(12);
        return b;
    }
  )");
  EXPECT_EQ(value, 12);
}

TEST(GenericFunctions, generic_function_with_capture) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 10;
        function add<T>(v: T) T {
            return v + x;
        }
        return add<i32>(5);
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(GenericFunctions, generic_function_capture_multiple_calls) {
  // Two calls to same specialization - capture remains unchanged
  // Note: Modifying captured variables between calls has a separate bug
  // that affects both generic and non-generic closures
  auto value = executeString(R"(
    function main() i32 {
        var x = 1;
        function add<T>(v: T) T {
            return v + x;
        }
        var a = add<i32>(10);
        var b = add<i32>(20);
        return a + b;
    }
  )");
  // a = 10 + 1 = 11, b = 20 + 1 = 21, total = 32
  EXPECT_EQ(value, 32);
}