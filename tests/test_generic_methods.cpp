// tests/test_generic_methods.cpp - Tests for generic class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

TEST(GenericMethods, generic_identity_method) {
  auto value = executeString(R"(
    class Util {
      function identity<T>(x: T) T {
        return x;
      }
    }

    function main() i32 {
        var u = Util();
        return u.identity<i32>(42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericMethods, generic_class_with_generic_method) {
  auto value = executeString(R"(
    function foo<T>(x: T) T {
      return x;
    }

    class Test<T> {
      function returnX<U>(x: U, y: T) U {
        return x;
      }
      function returnY<U>(x: U, y: T) T {
        return foo<T>(y);
      }
    }

    function main() f64 {
        var t = Test<f64>();
        var x = t.returnY<i32>(42, 3.14);
        return x;
    }
  )");
  EXPECT_EQ(value, 3.14);
}

TEST(GenericMethods, normal_method_calls_generic_function) {
  auto value = executeString(R"(
    function foo<T>(x: T) {
      return;
    }

    class Matrix<T> {
      function set(idx: i32, value: T) void {
          foo<T>(value);
      }
    }

    function main() i32 {
        var t = Matrix<i32>();
        t.set(0, 42);
        return 42;
    }
  )");
  EXPECT_EQ(value, 42);
}
