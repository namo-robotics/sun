// tests/functions/generic/test_methods.cpp - Tests for generic class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

TEST(Functions_Generic_Methods, generic_identity_method) {
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

TEST(Functions_Generic_Methods, generic_class_with_generic_method) {
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

// ============================================================================
// Inferred type arguments
// ============================================================================

TEST(Functions_Generic_Methods, type_argument_inferred_from_argument) {
  auto value = executeString(R"(
    class Conv {
      function init() {}
      function twice<U>(x: U) U { return x + x; }
    }
    function main() i32 {
        var c = Conv();
        return c.twice(21);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Functions_Generic_Methods, leading_type_arguments_given_rest_inferred) {
  auto value = executeString(R"(
    class Conv {
      function init() {}
      function as<R, U>(x: U) R { return _convert<R>(x); }
    }
    function main() i32 {
        var c = Conv();
        var ms: i64 = 300;
        return c.as<i32>(ms);
    }
  )");
  EXPECT_EQ(value, 300);
}

TEST(Functions_Generic_Methods, inference_on_generic_class_method) {
  auto value = executeString(R"(
    class Box<T> {
      var v: T;
      function init(v: T) { this.v = v; }
      function as<R, U>(x: U) R { return _convert<R>(x); }
      function twice<U>(x: U) U { return x + x; }
    }
    function main() i32 {
        var b = Box<i64>(1);
        var ms: i64 = 200;
        return b.as<i32>(ms) + b.twice(21);
    }
  )");
  EXPECT_EQ(value, 242);
}

TEST(Functions_Generic_Methods, given_type_argument_wins_over_argument_type) {
  auto value = executeString(R"(
    class W {
      function init() {}
      function width<A, B>(a: A, b: B) i64 { return _sizeof<A>() * 10 + _sizeof<B>(); }
    }
    function main() i64 {
        var w = W();
        return w.width<i64>(1, 2);   // A is i64 as written, B is i32 from the literal
    }
  )");
  EXPECT_EQ(value, 84);
}

TEST(Functions_Generic_Methods, inference_through_ref_parameter) {
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init(x: i32) { this.x = x; this.y = 0; }
    }
    class Reader {
      function init() {}
      function size_of<U>(p: ref U) i64 { return _sizeof<U>(); }
    }
    function main() i64 {
        var r = Reader();
        var p = Point(7);
        return r.size_of(p);   // U = Point, bound through the ref parameter
    }
  )");
  EXPECT_EQ(value, 8);
}

TEST(Functions_Generic_Methods, inference_inside_generic_class_body) {
  auto value = executeString(R"(
    class Box<T> {
      var v: T;
      function init(v: T) { this.v = v; }
      function twice<U>(x: U) U { return x + x; }
      function doubled() T { return this.twice(this.v); }
    }
    function main() i32 {
        var b = Box<i32>(21);
        return b.doubled();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Functions_Generic_Methods, uninferable_type_argument_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class F {
      function init() {}
      function make<T>() T { return _convert<T>(0); }
    }
    function main() i32 { var f = F(); return f.make(); }
  )"),
                                "Cannot infer type argument 'T' of generic method 'F.make'");
}

TEST(Functions_Generic_Methods, partial_type_arguments_uninferable_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class F {
      function init() {}
      function make<T, U>(n: T) U { return _convert<U>(n); }
    }
    function main() i32 { var f = F(); return f.make<i32>(1); }
  )"),
                                "Cannot infer type argument 'U'");
}

TEST(Functions_Generic_Methods, argument_mismatching_given_type_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Point { var x: i32; function init(x: i32) { this.x = x; } }
    class F {
      function init() {}
      function keep<T>(x: ref T) i32 { return 1; }
    }
    function main() i32 {
        var f = F();
        var n: i64 = 1;
        return f.keep<Point>(n);
    }
  )"),
                                "Type mismatch in argument 1");
}

TEST(Functions_Generic_Methods, normal_method_calls_generic_function) {
  auto value = executeString(R"(
    function foo<T>(x: T) void {
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
