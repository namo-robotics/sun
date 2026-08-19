// tests/lambdas/test_bound_methods.cpp - Class methods as first-class values
//
// Covers: obj.method in value position (bound method references) — passing
// as lambda-typed arguments, storing in variables, overload disambiguation
// via expected type, throwing methods, and rejection of ambiguous/generic
// references. The receiver is captured by reference: calls through the
// bound value mutate the original object.

#include <gtest/gtest.h>

#include "execution_utils.h"

TEST(Lambdas_BoundMethods, pass_method_as_callback) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() { this.count = 0; }
        function add(amount: i32) i32 {
            this.count = this.count + amount;
            return this.count;
        }
    }

    function apply(f: (i32) i32, x: i32) i32 {
        return f(x);
    }

    function main() i32 {
        var c = Counter();
        return apply(c.add, 5);
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Lambdas_BoundMethods, receiver_mutated_through_callback) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() { this.count = 0; }
        function add(amount: i32) i32 {
            this.count = this.count + amount;
            return this.count;
        }
    }

    function apply(f: (i32) i32, x: i32) i32 {
        return f(x);
    }

    function main() i32 {
        var c = Counter();
        apply(c.add, 5);
        apply(c.add, 7);
        return c.count;
    }
  )");
  EXPECT_EQ(value, 12);
}

TEST(Lambdas_BoundMethods, stored_in_var_then_called) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() { this.count = 0; }
        function add(amount: i32) i32 {
            this.count = this.count + amount;
            return this.count;
        }
    }

    function main() i32 {
        var c = Counter();
        var f = c.add;
        f(1);
        f(2);
        return c.count;
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Lambdas_BoundMethods, stored_with_type_annotation) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() { this.count = 0; }
        function add(amount: i32) i32 {
            this.count = this.count + amount;
            return this.count;
        }
    }

    function main() i32 {
        var c = Counter();
        var f: (i32) i32 = c.add;
        return f(9);
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(Lambdas_BoundMethods, this_method_as_callback) {
  auto value = executeString(R"(
    function apply(f: (i32) i32, x: i32) i32 {
        return f(x);
    }

    class Machine {
        var total: i32;
        function init() { this.total = 0; }
        function step(x: i32) i32 {
            this.total = this.total + x;
            return this.total;
        }
        function run() i32 {
            return apply(this.step, 11);
        }
    }

    function main() i32 {
        var m = Machine();
        return m.run();
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Lambdas_BoundMethods, ref_receiver) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() { this.count = 0; }
        function add(amount: i32) i32 {
            this.count = this.count + amount;
            return this.count;
        }
    }

    function apply(f: (i32) i32, x: i32) i32 {
        return f(x);
    }

    function bump(c: ref Counter) i32 {
        return apply(c.add, 4);
    }

    function main() i32 {
        var c = Counter();
        bump(c);
        bump(c);
        return c.count;
    }
  )");
  EXPECT_EQ(value, 8);
}

TEST(Lambdas_BoundMethods, overload_picked_by_annotation) {
  auto value = executeString(R"(
    class Calc {
        var last: i32;
        function init() { this.last = 0; }
        function add(x: i32) i32 {
            this.last = this.last + x;
            return this.last;
        }
        function add(x: f64) i32 {
            return 999;
        }
    }

    function main() i32 {
        var c = Calc();
        var f: (i32) i32 = c.add;
        return f(7);
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Lambdas_BoundMethods, overload_picked_by_param_context) {
  auto value = executeString(R"(
    class Calc {
        var last: i32;
        function init() { this.last = 0; }
        function add(x: i32) i32 {
            this.last = this.last + x;
            return this.last;
        }
        function add(x: f64) i32 {
            return 999;
        }
    }

    function apply(f: (i32) i32, x: i32) i32 {
        return f(x);
    }

    function main() i32 {
        var c = Calc();
        return apply(c.add, 6);
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(Lambdas_BoundMethods, ambiguous_overload_without_context) {
  EXPECT_THROW(executeString(R"(
    class Calc {
        function init() {}
        function add(x: i32) i32 { return x; }
        function add(x: f64) i32 { return 0; }
    }

    function main() i32 {
        var c = Calc();
        var f = c.add;
        return 0;
    }
  )"),
               SunError);
}

TEST(Lambdas_BoundMethods, generic_method_reference_rejected) {
  EXPECT_THROW(executeString(R"(
    class Box {
        function init() {}
        function unwrap<T>(x: T) T { return x; }
    }

    function main() i32 {
        var b = Box();
        var f = b.unwrap;
        return 0;
    }
  )"),
               SunError);
}

TEST(Lambdas_BoundMethods, throwing_method_into_throwing_param) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Parser {
        var errors: i32;
        function init() { this.errors = 0; }
        function parse(x: i32) i32, IError {
            if (x < 0) {
                this.errors = this.errors + 1;
                throw Error(1, "negative");
            }
            return x * 2;
        }
    }

    function run_guarded(f: (i32) i32, IError, x: i32) i32 {
        try {
            return f(x);
        } catch (e: IError) {
            return -1;
        }
    }

    function main() i32 {
        var p = Parser();
        var a = run_guarded(p.parse, 21);   // 42
        var b = run_guarded(p.parse, -5);   // -1
        return a + b + p.errors;            // 42 - 1 + 1
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Lambdas_BoundMethods, nonthrowing_method_into_throwing_param) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Doubler {
        function init() {}
        function twice(x: i32) i32 { return x * 2; }
    }

    function run_guarded(f: (i32) i32, IError, x: i32) i32 {
        try {
            return f(x);
        } catch (e: IError) {
            return -1;
        }
    }

    function main() i32 {
        var d = Doubler();
        return run_guarded(d.twice, 50);
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(Lambdas_BoundMethods, stored_throwing_method_uncaught_rejected) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;

    class Parser {
        function init() {}
        function parse(x: i32) i32, IError {
            if (x < 0) { throw Error(1, "negative"); }
            return x;
        }
    }

    function main() i32 {
        var p = Parser();
        var f = p.parse;
        return f(1);   // call outside try / ', IError' context
    }
  )"),
               SunError);
}

TEST(Lambdas_BoundMethods, bound_method_call_in_loop) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init() { this.count = 0; }
        function add(amount: i32) i32 {
            this.count = this.count + amount;
            return this.count;
        }
    }

    function main() i32 {
        var c = Counter();
        var f = c.add;
        var i = 0;
        while (i < 10) {
            f(1);
            i = i + 1;
        }
        return c.count;
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(Lambdas_BoundMethods, stdlib_class_method_as_value) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function apply(f: (i32) void) void {
        f(42);
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i32>(allocator, 8);
        apply(v.push);
        return v.get_unchecked(0);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Lambdas_BoundMethods, deinit_runs_once_with_bound_method) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Tracker {
        var alloc: HeapAllocator;
        var data: raw_ptr<u8>;
        var hits: i32;

        function init(allocator: ref HeapAllocator) {
            this.alloc = allocator.copy();
            this.data = this.alloc.alloc_raw(4);
            this.hits = 0;
        }

        function hit(x: i32) i32 {
            this.hits = this.hits + x;
            return this.hits;
        }

        function deinit() void {
            unsafe { _free(this.data); };
        }
    }

    function apply(f: (i32) i32, x: i32) i32 {
        return f(x);
    }

    function main() i32 {
        var alloc = HeapAllocator();
        var t = Tracker(alloc);
        apply(t.hit, 3);
        apply(t.hit, 4);
        return t.hits;
    }
  )");
  EXPECT_EQ(value, 7);
}
