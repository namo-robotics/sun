// tests/test_lambda_params.cpp - Lambdas as function/method parameters
//
// Covers: lambda-typed params ((T) R), ref class params in lambda
// signatures, and inline lambda literals as call arguments.

#include <gtest/gtest.h>

#include "execution_utils.h"

TEST(LambdaParamTest, lambda_param_by_value) {
  auto value = executeString(R"(
    function apply_twice(f: (i32) i32, x: i32) i32 {
        return f(f(x));
    }

    function main() i32 {
        var add3 = lambda (x: i32) i32 { return x + 3; };
        return apply_twice(add3, 10);
    }
  )");
  EXPECT_EQ(value, 16);
}

TEST(LambdaParamTest, lambda_literal_argument) {
  auto value = executeString(R"(
    function apply_twice(f: (i32) i32, x: i32) i32 {
        return f(f(x));
    }

    function main() i32 {
        return apply_twice(lambda (x: i32) i32 { return x * 2; }, 5);
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(LambdaParamTest, lambda_ref_class_param) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() {
            this.count = 0;
        }
        function bump() void {
            this.count = this.count + 1;
        }
    }

    function with_counter(handler: (ref Counter) void) i32 {
        var c = Counter();
        handler(c);
        handler(c);
        return c.count;
    }

    function main() i32 {
        var h = lambda (c: ref Counter) void {
            c.bump();
        };
        return with_counter(h);
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(LambdaParamTest, lambda_ref_class_param_literal) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() {
            this.count = 0;
        }
        function bump() void {
            this.count = this.count + 1;
        }
    }

    function with_counter(handler: (ref Counter) void) i32 {
        var c = Counter();
        handler(c);
        handler(c);
        handler(c);
        return c.count;
    }

    function main() i32 {
        return with_counter(lambda (c: ref Counter) void {
            c.bump();
        });
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(LambdaParamTest, method_lambda_param) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() {
            this.count = 0;
        }
        function bump() void {
            this.count = this.count + 1;
        }
    }

    class Runner {
        var dummy: i32;
        function init() {
            this.dummy = 0;
        }
        function run_twice(handler: (ref Counter) void) i32 {
            var c = Counter();
            handler(c);
            handler(c);
            return c.count;
        }
    }

    function main() i32 {
        var r = Runner();
        return r.run_twice(lambda (c: ref Counter) void {
            c.bump();
        });
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(LambdaParamTest, lambda_param_forwarded_between_methods) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() {
            this.count = 0;
        }
        function bump() void {
            this.count = this.count + 1;
        }
    }

    class Runner {
        var dummy: i32;
        function init() {
            this.dummy = 0;
        }
        function outer(handler: (ref Counter) void) i32 {
            return this.inner(handler);
        }
        function inner(handler: (ref Counter) void) i32 {
            var c = Counter();
            handler(c);
            return c.count;
        }
    }

    function main() i32 {
        var r = Runner();
        return r.outer(lambda (c: ref Counter) void {
            c.bump();
        });
    }
  )");
  EXPECT_EQ(value, 1);
}
