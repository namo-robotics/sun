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
