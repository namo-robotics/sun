// tests/lambdas/generic/test_lambdas.cpp - Lambdas in generic contexts
//
// Sun lambda literals may declare lifetime parameters, but not type parameters
// (see the parse-error test at the bottom). What this file covers is the
// feature that does exist - lambdas written inside a generic function or
// class, where the enclosing type parameter is in scope.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Lambdas_Generic, lambda_body_uses_the_type_parameter) {
  auto value = executeString(R"(
    function apply<T>(x: T) T {
        var f = lambda (v: T) T { return v; };
        return f(x);
    }

    function main() i32 {
        return apply<i32>(11);
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Lambdas_Generic, lambda_param_typed_with_the_type_parameter) {
  auto value = executeString(R"(
    function apply<T>(x: T, f: (T) -> T) T {
        return f(x);
    }

    function main() i32 {
        return apply<i32>(5, lambda (v: i32) i32 { return v * 3; });
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(Lambdas_Generic, lambda_captures_a_generic_value_by_ref) {
  auto value = executeString(R"(
    function add_k<T>(x: T, k: T) T {
        var f = lambda [ref k] (v: T) T { return v + k; };
        return f(x);
    }

    function main() i32 {
        return add_k<i32>(3, 4);
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Lambdas_Generic, lambda_closes_over_the_generic_parameter) {
  auto value = executeString(R"(
    function outer<T>(x: T) T {
        var f = lambda [ref x] () T { return x; };
        return f();
    }

    function main() i32 {
        return outer<i32>(17);
    }
  )");
  EXPECT_EQ(value, 17);
}

TEST(Lambdas_Generic, lambda_calls_another_lambda_in_the_same_body) {
  auto value = executeString(R"(
    function build<T>(x: T) T {
        var f = lambda (v: T) T { return v; };
        var g = lambda (v: T) T { return f(v); };
        return g(x);
    }

    function main() i32 {
        return build<i32>(13);
    }
  )");
  EXPECT_EQ(value, 13);
}

TEST(Lambdas_Generic, each_specialization_gets_its_own_lambda) {
  // twice<i32> and twice<f64> instantiate the same lambda at two types.
  auto value = executeString(R"(
    function twice<T>(x: T) T {
        var f = lambda (v: T) T { return v + v; };
        return f(x);
    }

    function main() i32 {
        var a = twice<i32>(4);
        var b = twice<f64>(1.5);
        if (b == 3.0) {
            return a;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 8);
}

TEST(Lambdas_Generic, lambda_in_a_generic_class_method) {
  auto value = executeString(R"(
    class Wrapper<T> {
        var v: T;
        init(v: T) { this.v = v; }
        method mapped() T {
            var f = lambda (x: T) T { return x + x; };
            return f(this.v);
        }
    }

    function main() i32 {
        var w = Wrapper<i32>(12);
        return w.mapped();
    }
  )");
  EXPECT_EQ(value, 24);
}

TEST(Lambdas_Generic, lambda_type_parameters_are_rejected) {
  // Lambdas accept lifetime binders, but `lambda <T> (...) ` is still a parse error.
  EXPECT_THROW(executeString(R"(
        function main() i32 {
            var f = lambda <T> (x: T) T { return x; };
            return 0;
        }
      )"),
               SunError);
}
