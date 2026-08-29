// tests/functions/test_functions.cpp - Plain (non-generic) named functions
//
// Covers: parameter passing, recursion, declaration order, overload
// resolution, and how class arguments move or borrow. Generic functions
// live in Functions_Generic; lambdas live in Lambdas_*.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Functions, by_value_primitive_param) {
  auto value = executeString(R"(
    function double_it(x: i32) i32 {
        return x * 2;
    }

    function main() i32 {
        return double_it(21);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Functions, multiple_params) {
  auto value = executeString(R"(
    function combine(a: i32, b: i32, c: i32) i32 {
        return a + b * c;
    }

    function main() i32 {
        return combine(2, 3, 4);
    }
  )");
  EXPECT_EQ(value, 14);
}

TEST(Functions, argument_is_a_copy_not_an_alias) {
  auto value = executeString(R"(
    function clobber(x: i32) i32 {
        x = 99;
        return x;
    }

    function main() i32 {
        var v = 5;
        clobber(v);
        return v;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Functions, call_before_definition) {
  // Declaration order does not matter at module level.
  auto value = executeString(R"(
    function main() i32 {
        return later(4);
    }

    function later(a: i32) i32 {
        return a * 2;
    }
  )");
  EXPECT_EQ(value, 8);
}

TEST(Functions, recursion) {
  auto value = executeString(R"(
    function fact(n: i32) i32 {
        if (n <= 1) {
            return 1;
        }
        return n * fact(n - 1);
    }

    function main() i32 {
        return fact(5);
    }
  )");
  EXPECT_EQ(value, 120);
}

TEST(Functions, mutual_recursion) {
  auto value = executeString(R"(
    function is_even(n: i32) bool {
        if (n == 0) {
            return true;
        }
        return is_odd(n - 1);
    }

    function is_odd(n: i32) bool {
        if (n == 0) {
            return false;
        }
        return is_even(n - 1);
    }

    function main() i32 {
        if (is_even(10)) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Functions, nested_calls) {
  auto value = executeString(R"(
    function inc(x: i32) i32 { return x + 1; }
    function twice(x: i32) i32 { return x * 2; }

    function main() i32 {
        return twice(inc(twice(5)));
    }
  )");
  EXPECT_EQ(value, 22);
}

TEST(Functions, void_return) {
  auto value = executeString(R"(
    var g: i32 = 0;

    function bump() void {
        g = g + 4;
    }

    function main() i32 {
        bump();
        bump();
        return g;
    }
  )");
  EXPECT_EQ(value, 8);
}

TEST(Functions, float_params_and_return) {
  auto value = executeString(R"(
    function half(x: f64) f64 {
        return x / 2.0;
    }

    function main() i32 {
        if (half(9.0) == 4.5) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Functions, ref_class_param_borrows) {
  // A ref parameter borrows, so the caller can keep using the value.
  auto value = executeString(R"(
    class Box {
        var v: i32;
        init(v: i32) { this.v = v; }
        function get() i32 { return this.v; }
    }

    function peek(b: ref Box) i32 {
        return b.get();
    }

    function main() i32 {
        var b = Box(6);
        return peek(b) + peek(b);
    }
  )");
  EXPECT_EQ(value, 12);
}

TEST(Functions, ref_class_param_can_mutate_caller_value) {
  auto value = executeString(R"(
    class Box {
        var v: i32;
        init(v: i32) { this.v = v; }
        function bump() void { this.v = this.v + 1; }
        function get() i32 { return this.v; }
    }

    function bump_twice(b: ref Box) void {
        b.bump();
        b.bump();
    }

    function main() i32 {
        var b = Box(5);
        bump_twice(b);
        return b.get();
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Functions, returns_class_by_value) {
  auto value = executeString(R"(
    class Box {
        var v: i32;
        init(v: i32) { this.v = v; }
        function get() i32 { return this.v; }
    }

    function make(n: i32) Box {
        return Box(n);
    }

    function main() i32 {
        var b = make(21);
        return b.get() * 2;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Functions, class_by_value_param_is_a_move) {
  auto value = executeString(R"(
    class Box {
        var v: i32;
        init(v: i32) { this.v = v; }
        function get() i32 { return this.v; }
    }

    function eat(b: Box) i32 {
        return b.get();
    }

    function main() i32 {
        var b = Box(6);
        return eat(b);
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(Functions, use_after_passing_class_by_value_is_error) {
  // Passing a class by value moves it; the caller cannot touch it again.
  EXPECT_THROW(executeString(R"(
        class Box {
            var v: i32;
            init(v: i32) { this.v = v; }
            function get() i32 { return this.v; }
        }

        function eat(b: Box) i32 {
            return b.get();
        }

        function main() i32 {
            var b = Box(6);
            var x = eat(b);
            return x + b.get();
        }
      )"),
               SunError);
}

TEST(Functions, overload_resolution_by_param_type) {
  auto value = executeString(R"(
    function f(a: i32) i32 { return 1; }
    function f(a: f64) i32 { return 2; }

    function main() i32 {
        return f(1.5);
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Functions, overload_resolution_by_arity) {
  auto value = executeString(R"(
    function f(a: i32) i32 { return 1; }
    function f(a: i32, b: i32) i32 { return 2; }

    function main() i32 {
        return f(1, 2);
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Functions, wrong_argument_count_is_error) {
  EXPECT_THROW(executeString(R"(
        function f(a: i32) i32 { return a; }

        function main() i32 {
            return f(1, 2);
        }
      )"),
               SunError);
}

TEST(Functions, wrong_argument_type_is_error) {
  EXPECT_THROW(executeString(R"(
        function f(a: i32) i32 { return a; }

        function main() i32 {
            return f(true);
        }
      )"),
               SunError);
}

TEST(Functions, unknown_function_is_error) {
  EXPECT_THROW(executeString(R"(
        function main() i32 {
            return nowhere(1);
        }
      )"),
               SunError);
}

TEST(Functions, binding_void_call_to_inferred_var_is_error) {
  // A call that returns nothing gives an inferred `var` nothing to hold;
  // the compiler used to reach codegen and trap (issue #86).
  try {
    executeString(R"(
        function nothing() void { }

        function main() i32 {
            var t = nothing();
            return 0;
        }
      )");
    FAIL() << "Expected SunError for binding a void result";
  } catch (const SunError& e) {
    std::string msg = e.what();
    EXPECT_TRUE(msg.find("'t'") != std::string::npos)
        << "Error should name the variable, got: " << msg;
  }
}

TEST(Functions, binding_void_call_to_typed_var_is_error) {
  EXPECT_THROW(executeString(R"(
        function nothing() void { }

        function main() i32 {
            var t: void = nothing();
            return 0;
        }
      )"),
               SunError);
}

TEST(Functions, narrowing_float_argument_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function half(x: f32) f32 { return x / 2.0; }
    function main() i32 {
        var d: f64 = 3.0;
        return _convert<i32>(half(d));
    }
  )"),
                                "expected f32, got f64");
}

TEST(Functions, widening_argument_conversions) {
  auto value = executeString(R"(
    function add(a: i64, b: f64) i64 { return a + _convert<i64>(b); }
    function main() i32 {
        var small: i16 = 40;
        var f: f32 = 2.0;
        return _convert<i32>(add(small, f));
    }
  )");
  EXPECT_EQ(value, 42);
}

// -------------------------------------------------------------------
// Returning a void expression
// -------------------------------------------------------------------
// `return f();` in a void function, where f is itself void. The expression
// is evaluated for its effect and there is no value to hand back. This is
// what lets a generic method forward a call whose result type is the type
// parameter, in the specialization where that parameter is void.

TEST(Functions, void_function_returns_a_void_call) {
  auto value = executeString(R"(
    var counter: i32 = 0;
    function bump() void { counter = counter + 1; }
    function forward() void { return bump(); }
    function main() i32 {
        forward();
        forward();
        return counter;
    }
  )");
  EXPECT_EQ(value, 2);
}

// The forwarded call still runs before the early exit it causes.
TEST(Functions, returning_a_void_call_stops_the_function) {
  auto value = executeString(R"(
    var counter: i32 = 0;
    function bump() void { counter = counter + 1; }
    function forward() void {
        return bump();
        counter = 100;
    }
    function main() i32 {
        forward();
        return counter;
    }
  )");
  EXPECT_EQ(value, 1);
}
