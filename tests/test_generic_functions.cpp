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

TEST(GenericFunctions, nested_generic_function) {
  auto value = executeString(R"(
    function main() i32 {
        function outer<T>(x: T) T {
            function inner<U>(y: U) U {
                return y + y;
            }
            return x + inner<T>(x);
        }
        return outer<i32>(5);
    }
  )");
  // inner(5) = 5+5 = 10, outer = 5 + 10 = 15
  EXPECT_EQ(value, 15);
}

TEST(GenericFunctions, inner_generic_function_with_capture) {
  auto value = executeString(R"(
    function main() i32 {
        function outer<T>(x: T) T {
            function inner<U>(y: U) U {
                return y + x;
            }
            return inner<i8>(3);
        }
        return outer<i32>(5);
    }
  )");
  // inner(3) = 3+5 = 8, outer = 8
  EXPECT_EQ(value, 8);
}
// ============================================================================
// Integer-literal arguments to explicit instantiations
// ============================================================================

TEST(GenericFunctions, literal_args_widened_to_i64) {
  auto value = executeString(R"(
    function scale <T> (v: T, by: T) T {
        return v * by;
    }

    function main() i64 {
        return scale<i64>(8, 9);
    }
  )");
  EXPECT_EQ(value, 72);
}

TEST(GenericFunctions, literal_args_narrowed_to_i8) {
  auto value = executeString(R"(
    function double_it <T> (v: T) T {
        return v + v;
    }

    function main() i32 {
        var r: i8 = double_it<i8>(3);
        if (r == 6) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(GenericFunctions, literal_arg_out_of_range_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function double_it <T> (v: T) T {
        return v + v;
    }

    function main() i32 {
        var r: i8 = double_it<i8>(300);
        return 0;
    }
  )"),
                                "cannot be represented");
}

// ============================================================================
// Generic bodies are analyzed in their definition scope, not the requester's
// ============================================================================

TEST(GenericFunctions, body_cannot_see_requesters_locals) {
  // `secret` is a local of main; the template never declared it
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function leak<T>(v: T) T { return v + secret; }
    function main() i32 {
        var secret: i32 = 5;
        return leak<i32>(1);
    }
  )"), "Unknown variable");
}

TEST(GenericFunctions, module_private_helper_reachable_from_two_requesters) {
  auto value = executeString(R"(
    public module a {
        function helper() i32 { return 10; }
        public function twice<T>(v: T) T { return v + helper() + helper(); }
    }
    using a;
    public module b {
        public function via_b() i32 { return twice<i32>(1); }
    }
    function main() i32 { return b.via_b() + twice<i32>(2); }
  )");
  EXPECT_EQ(value, 21 + 22);
}

TEST(GenericFunctions, nested_generic_with_capture_two_outer_specializations) {
  auto value = executeString(R"(
    function outer<T>(base: T) T {
        function inner<U>(v: U) U { return v + base; }
        return inner<T>(base) + inner<T>(base);
    }
    function main() i32 {
        var a: i32 = outer<i32>(3);     // 12
        var b: i64 = outer<i64>(5);     // 20
        return a + b;
    }
  )");
  EXPECT_EQ(value, 32);
}

// ============================================================================
// Call sites ahead of the definition (issue #78)
// ============================================================================

TEST(GenericFunctions, called_from_generic_class_method_defined_above_it) {
  auto value = executeString(R"(
    class Box<T> {
        var v: T;
        function init(v: T) { this.v = v; }
        function get() T { return helper<T>(this.v); }
    }
    function helper<T>(x: T) T { return x + x; }
    function main() i32 {
        var b = Box<i32>(21);
        return b.get();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, called_from_class_method_defined_above_it) {
  auto value = executeString(R"(
    class Plain {
        var n: i32;
        function init(n: i32) { this.n = n; }
        function get() i32 { return helper<i32>(this.n); }
    }
    function helper<T>(x: T) T { return x + x; }
    function main() i32 {
        var p = Plain(21);
        return p.get();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, called_from_function_defined_above_it) {
  auto value = executeString(R"(
    function caller() i32 { return helper<i32>(21); }
    function helper<T>(x: T) T { return x + x; }
    function main() i32 { return caller(); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, generic_calls_generic_defined_below_it) {
  auto value = executeString(R"(
    function outer<T>(x: T) T { return inner<T>(x) + inner<T>(x); }
    function inner<T>(x: T) T { return x; }
    function main() i32 { return outer<i32>(21); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, mutually_ordered_specializations_share_one_symbol) {
  // Both call sites — the one above the definition and the one below — must
  // resolve to the same specialization.
  auto value = executeString(R"(
    function before() i32 { return helper<i32>(20); }
    function helper<T>(x: T) T { return x + 1; }
    function after() i32 { return helper<i32>(20); }
    function main() i32 { return before() + after(); }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Throwing generic functions
// ============================================================================

TEST(GenericFunctions, throwing_generic_is_catchable) {
  auto value = executeString(R"(
    class Boom implements IError {
        function init() {}
        function code() i32 { return 1; }
        function message() static_ptr<u8> { return "boom"; }
    }
    function risky<T>(x: T) i32, IError { throw Boom(); }
    function main() i32 {
        try { return risky<i32>(1); } catch (e: IError) { return 42; }
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, throwing_generic_is_catchable_from_class_method) {
  auto value = executeString(R"(
    class Boom implements IError {
        function init() {}
        function code() i32 { return 1; }
        function message() static_ptr<u8> { return "boom"; }
    }
    class Box<T> {
        var v: T;
        function init(v: T) { this.v = v; }
        function get() i32 {
            try { return risky<T>(this.v); } catch (e: IError) { return 42; }
        }
    }
    function risky<T>(x: T) i32, IError { throw Boom(); }
    function main() i32 { var b = Box<i32>(1); return b.get(); }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Parameter shapes: ref T, and generic classes named in a signature
// ============================================================================

TEST(GenericFunctions, ref_type_parameter_receives_an_address) {
  auto value = executeString(R"(
    function bump<T>(x: ref T) i32 { return 42; }
    function main() i32 {
        var v: i32 = 3;
        return bump<i32>(v);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, ref_type_parameter_from_generic_class_method) {
  auto value = executeString(R"(
    function bump<T>(x: ref T) i32 { return 42; }
    class Box<T> {
        var v: T;
        function init(v: T) { this.v = v; }
        function get() i32 { var local: T = this.v; return bump<T>(local); }
    }
    function main() i32 { var b = Box<i32>(1); return b.get(); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, parameter_naming_a_generic_class) {
  auto value = executeString(R"(
    class Pair<A> {
        var a: A;
        function init(a: A) { this.a = a; }
        function get() A { return this.a; }
    }
    function unwrap<T>(p: ref Pair<T>) T { return p.get(); }
    function main() i32 {
        var p = Pair<i32>(42);
        return unwrap<i32>(p);
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Module-qualified generic calls
// ============================================================================

TEST(GenericFunctions, module_qualified_call) {
  auto value = executeString(R"(
    public module m {
        public function twice<T>(x: T) T { return x + x; }
    }
    function main() i32 { return m.twice<i32>(21); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, nested_module_qualified_call_from_generic_method) {
  auto value = executeString(R"(
    public module a {
        public module b {
            public function pick<T>(x: T, y: T) T { return x; }
        }
    }
    class Box<T> {
        var v: T;
        function init(v: T) { this.v = v; }
        function get() T { return a.b.pick<T>(this.v, this.v); }
    }
    function main() i32 {
        var box = Box<i32>(20);
        return box.get() + a.b.pick<i32>(22, 0);
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Type arguments inferred from the call's arguments
// ============================================================================

TEST(GenericFunctions, type_argument_inferred_from_argument) {
  auto value = executeString(R"(
    function identity<T>(x: T) T { return x; }
    function main() i32 { return identity(42); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, type_arguments_inferred_for_each_parameter) {
  auto value = executeString(R"(
    function add<T>(a: T, b: T) T { return a + b; }
    function first<A, B>(a: A, b: B) A { return a; }
    function main() i32 { return add(20, 21) + first(1, true); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, type_argument_inferred_through_ref_parameter) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        function init(x: i32) { this.x = x; }
        function get() i32 { return this.x; }
    }
    function peek<T>(v: ref T) i32 { return 42; }
    function main() i32 {
        var p = Point(1);
        return peek(p);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, type_argument_inferred_from_generic_class_argument) {
  auto value = executeString(R"(
    class Pair<A> {
        var a: A;
        function init(a: A) { this.a = a; }
        function get() A { return this.a; }
    }
    function unwrap<T>(p: ref Pair<T>) T { return p.get(); }
    function main() i32 {
        var p = Pair<i32>(42);
        return unwrap(p);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, inference_from_imported_module) {
  auto value = executeString(R"(
    public module m {
        public function twice<T>(x: T) T { return x + x; }
    }
    using m;
    function main() i32 { return twice(21); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericFunctions, uninferable_type_argument_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function nothing<T>(n: i32) i32 { return n; }
    function main() i32 { return nothing(1); }
  )"),
                                "Cannot infer type argument 'T'");
}
