// tests/lambdas/test_params.cpp - Lambdas as function/method parameters
//
// Covers: lambda-typed params ((T) R), ref class params in lambda
// signatures, and inline lambda literals as call arguments.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Lambdas_Params, lambda_param_by_value) {
  auto value = executeString(R"(
    function apply_twice(f: (i32) => i32, x: i32) i32 {
        return f(f(x));
    }

    function main() i32 {
        var add3 = (x: i32) => i32 { return x + 3; };
        return apply_twice(add3, 10);
    }
  )");
  EXPECT_EQ(value, 16);
}

TEST(Lambdas_Params, lambda_literal_argument) {
  auto value = executeString(R"(
    function apply_twice(f: (i32) => i32, x: i32) i32 {
        return f(f(x));
    }

    function main() i32 {
        return apply_twice((x: i32) => i32 { return x * 2; }, 5);
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(Lambdas_Params, lambda_ref_class_param) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        init() {
            this.count = 0;
        }
        method bump() void {
            this.count = this.count + 1;
        }
    }

    function with_counter(handler: (ref Counter) => void) i32 {
        var c = Counter();
        handler(c);
        handler(c);
        return c.count;
    }

    function main() i32 {
        var h = (c: ref Counter) => void {
            c.bump();
        };
        return with_counter(h);
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Lambdas_Params, lambda_ref_class_param_literal) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        init() {
            this.count = 0;
        }
        method bump() void {
            this.count = this.count + 1;
        }
    }

    function with_counter(handler: (ref Counter) => void) i32 {
        var c = Counter();
        handler(c);
        handler(c);
        handler(c);
        return c.count;
    }

    function main() i32 {
        return with_counter((c: ref Counter) => void {
            c.bump();
        });
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Lambdas_Params, method_lambda_param) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        init() {
            this.count = 0;
        }
        method bump() void {
            this.count = this.count + 1;
        }
    }

    class Runner {
        var dummy: i32;
        init() {
            this.dummy = 0;
        }
        method run_twice(handler: (ref Counter) => void) i32 {
            var c = Counter();
            handler(c);
            handler(c);
            return c.count;
        }
    }

    function main() i32 {
        var r = Runner();
        return r.run_twice((c: ref Counter) => void {
            c.bump();
        });
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Lambdas_Params, lambda_param_forwarded_between_methods) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        init() {
            this.count = 0;
        }
        method bump() void {
            this.count = this.count + 1;
        }
    }

    class Runner {
        var dummy: i32;
        init() {
            this.dummy = 0;
        }
        method outer(handler: (ref Counter) => void) i32 {
            return this.inner(handler);
        }
        method inner(handler: (ref Counter) => void) i32 {
            var c = Counter();
            handler(c);
            return c.count;
        }
    }

    function main() i32 {
        var r = Runner();
        return r.outer((c: ref Counter) => void {
            c.bump();
        });
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Throwing lambdas: (args) => T throws IError { ... }
// ============================================================================

TEST(Lambdas_Throwing, throw_and_catch) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var risky = (x: i32) => i32 throws IError {
            if (x < 0) { throw Error(1, "negative"); }
            return x * 2;
        };

        try {
            var a = risky(5);
            var b = risky(-1);
            return a + b;
        } catch (e: IError) {
            return 42;
        }
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Lambdas_Throwing, throwing_lambda_as_param) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function run_guarded(f: (i32) => i32 throws IError, x: i32) i32 {
        try {
            return f(x);
        } catch (e: IError) {
            return -1;
        }
    }

    function main() i32 {
        var risky = (x: i32) => i32 throws IError {
            if (x < 0) { throw Error(1, "negative"); }
            return x * 2;
        };
        var a = run_guarded(risky, 21);   // 42
        var b = run_guarded(risky, -5);   // -1
        return a + b;
    }
  )");
  EXPECT_EQ(value, 41);
}

TEST(Lambdas_Throwing, non_throwing_into_throwing_param) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function run_guarded(f: (i32) => i32 throws IError, x: i32) i32 {
        try {
            return f(x);
        } catch (e: IError) {
            return -1;
        }
    }

    function main() i32 {
        var safe = (x: i32) => i32 {
            return x + 100;
        };
        return run_guarded(safe, 1);
    }
  )");
  EXPECT_EQ(value, 101);
}

TEST(Lambdas_Throwing, uncaught_call_rejected) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var risky = (x: i32) => i32 throws IError {
            if (x < 0) { throw Error(1, "negative"); }
            return x;
        };
        return risky(1);
    }
  )"),
               SunError);
}

TEST(Lambdas_Throwing, throw_in_non_throwing_lambda_rejected) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var bad = (x: i32) => i32 {
            throw Error(1, "boom");
        };
        return bad(1);
    }
  )"),
               SunError);
}

TEST(Lambdas_Params, extra_argument_to_zero_parameter_lambda_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
        var f = () => i32 { return 3; };
        return f(9);
    }
  )"),
                                "'f' expects 0 arguments, got 1");
}
