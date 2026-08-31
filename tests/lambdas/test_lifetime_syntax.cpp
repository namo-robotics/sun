// tests/lambdas/test_lifetime_syntax.cpp - Lifetime annotation syntax
//
// Lifetimes are Rust-style leading-apostrophe names declared in the angle
// brackets ('function pick<'a>', 'class Bus<'a>') and used on lambda types
// ('<'a>(i32) -> i32'), references ('ref 'a T'), and type applications
// ('Bus<'this>'). This file covers the spellings the parser accepts, the
// names semantic analysis rejects, and the character literals the new
// apostrophe token must not disturb. Lifetime CHECKING has its own tests;
// here the annotations only need to parse, resolve, and round-trip.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// ============================================================================
// Accepted spellings
// ============================================================================

// A function declares 'a and uses it on a parameter's lambda type.
TEST(Lambdas_LifetimeSyntax, function_lifetime_param_accepted) {
  auto value = executeString(R"(
    function take<'a>(f: <'a>(i32) -> i32, x: i32) i32 {
        return f(x);
    }
    function main() i32 {
        var b = 40;
        return take(lambda [ref b](n: i32) i32 { return b + n; }, 2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// Lifetimes and type parameters share the angle brackets, lifetimes first —
// and the lifetime does not count toward generic arity, so the explicit
// instantiation still names only the type argument.
TEST(Lambdas_LifetimeSyntax, mixed_lifetime_and_type_params_accepted) {
  auto value = executeString(R"(
    function combine<'a, T>(f: <'a>(T) -> T, x: T) T {
        return f(x);
    }
    function main() i32 {
        var b = 40;
        return combine<i32>(lambda [ref b](n: i32) i32 { return b + n; }, 2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A class declares a lifetime and uses it on a field and a method; a
// second class binds it to its receiver with Bus<'this>.
TEST(Lambdas_LifetimeSyntax, class_lifetime_and_this_application_accepted) {
  auto value = executeString(R"(
    class Bus<'a> {
        var cb: <'a>(i32) -> i32;
        init() { this.cb = lambda (x: i32) i32 { return x; }; }
        public method subscribe(cb: <'a>(i32) -> i32) void { this.cb = cb; return; }
        public method publish(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var total: i32;
        init() { this.total = 0; }
        public method onMsg(x: i32) i32 { this.total = this.total + x; return this.total; }
        public method attach(bus: ref Bus<'this>) void { bus.subscribe(this.onMsg); return; }
    }
    function main() i32 {
        var n = Node();
        var bus = Bus();
        n.attach(bus);
        bus.publish(40);
        bus.publish(2);
        return n.total;
    }
  )");
  EXPECT_EQ(value, 42);
}

// 'this on a method's own parameter, with a reference spelling ref 'a T.
TEST(Lambdas_LifetimeSyntax, ref_lifetime_and_this_param_accepted) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        init() { this.count = 40; }
        public method feed(g: <'this>(i32) -> i32) i32 { return g(this.count); }
    }
    function bump<'a>(c: ref 'a Counter, by: i32) i32 {
        return c.count + by;
    }
    function main() i32 {
        var c = Counter();
        var two = 2;
        var viaThis = c.feed(lambda [ref two](n: i32) i32 { return n + two; });
        return bump(c, viaThis - c.count);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A lambda may bind a lifetime for relationships among its arguments. The
// binder appears before its optional capture list.
TEST(Lambdas_LifetimeSyntax, lambda_lifetime_parameter_with_capture) {
  auto value = executeString(R"(
    class Box {
        var f: <'this>() -> i32;
        init() { this.f = lambda () i32 { return 0; }; }
        public method set(f: <'this>() -> i32) void {
            this.f = f;
            return;
        }
        public method call() i32 {
            var callback = this.f;
            return callback();
        }
    }
    function main() i32 {
        var kept = 42;
        var box = Box();
        var touched = 0;
        var store = lambda<'a> [ref touched](
            f: <'a>() -> i32,
            destination: ref 'a Box
        ) void {
            var forwarded: <'a>() -> i32 = f;
            destination.set(forwarded);
            touched = touched + 1;
            return;
        };
        store(lambda [ref kept]() i32 { return kept; }, box);
        return box.call();
    }
  )");
  EXPECT_EQ(value, 42);
}

// A lambda's lifetime binder may connect a callback argument to its result.
TEST(Lambdas_LifetimeSyntax, lambda_lifetime_parameter_on_return) {
  auto value = executeString(R"(
    function main() i32 {
        var kept = 42;
        var identity = lambda<'a>(
            f: <'a>() -> i32
        ) <'a>() -> i32 {
            return f;
        };
        var selected = identity(lambda [ref kept]() i32 { return kept; });
        return selected();
    }
  )");
  EXPECT_EQ(value, 42);
}

// The apostrophe token must not disturb character literals around it.
TEST(Lambdas_LifetimeSyntax, char_literals_unaffected) {
  auto value = executeString(R"(
    function classify<'a>(f: <'a>() -> char) i32 {
        var c: char = f();
        if (c == 'a') { return 42; }
        return 0;
    }
    function main() i32 {
        var c: char = 'a';
        return classify(lambda () char { return c; });
    }
  )");
  EXPECT_EQ(value, 42);
}

// An anonymous callback parameter may be called without being related to an
// unrelated ref argument. This is the false positive the old marker caused.
TEST(Lambdas_LifetimeSyntax, anonymous_lifetime_does_not_tie_arguments) {
  auto value = executeString(R"(
    class Holder {
        var count: i32;
        init() { this.count = 0; }
        public method bump() void { this.count = this.count + 1; return; }
    }
    class Node {
        var value: i32;
        init(value: i32) { this.value = value; }
        public method onMsg(x: i32) i32 { return this.value + x; }
    }
    function apply(cb: <'_>(i32) -> i32, dst: ref Holder) i32 {
        dst.bump();
        return cb(1);
    }
    function main() i32 {
        var h = Holder();
        if (true) {
            var n = Node(41);
            return apply(n.onMsg, h);
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

// An anonymous lifetime promises no relationship to the receiver, so a
// callback-storing method must name the receiver lifetime explicitly.
TEST(Lambdas_LifetimeSyntax, anonymous_lifetime_store_rejected) {
  EXPECT_THROW(executeString(R"(
    class Holder {
        var cb: <'_>() -> i32;
        init() { this.cb = lambda () i32 { return 0; }; }
        public method subscribe(cb: <'_>() -> i32) void {
            this.cb = cb;
            return;
        }
    }
    function main() i32 { return 0; }
  )"),
               SunError);
}

// ============================================================================
// Rejected spellings and names
// ============================================================================


// A lambda lifetime binder follows the same duplicate-name rule as a function.
TEST(Lambdas_LifetimeSyntax, duplicate_lambda_lifetime_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
        var f = lambda<'a, 'a>(g: <'a>() -> i32) i32 {
            return g();
        };
        return 0;
    }
  )"),
                                "duplicate lifetime parameter");
}

// A lifetime nobody declared is an error, not an implicit parameter.
TEST(Lambdas_LifetimeSyntax, undeclared_lifetime_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function f(g: <'a>() -> i32) i32 { return g(); }
    function main() i32 { return 0; }
  )"),
                                "use of undeclared lifetime 'a");
}

// 'this belongs to class and interface members only.
TEST(Lambdas_LifetimeSyntax, this_lifetime_outside_class_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function f(g: <'this>() -> i32) i32 { return g(); }
    function main() i32 { return 0; }
  )"),
                                "only usable inside class and interface");
}

// The anonymous lifetime is builtin and cannot be declared.
TEST(Lambdas_LifetimeSyntax, declaring_anonymous_lifetime_rejected) {
  EXPECT_THROW(executeString(R"(
    function f<'_>(g: <'_>() -> i32) i32 { return g(); }
    function main() i32 { return 0; }
  )"),
               SunError);
}

// 'this is builtin and cannot be declared.
TEST(Lambdas_LifetimeSyntax, declaring_this_lifetime_rejected) {
  EXPECT_THROW(executeString(R"(
    function f<'this>(g: <'this>() -> i32) i32 { return g(); }
    function main() i32 { return 0; }
  )"),
               SunError);
}

// Duplicate declarations collide.
TEST(Lambdas_LifetimeSyntax, duplicate_lifetime_param_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function f<'a, 'a>(g: <'a>() -> i32) i32 { return g(); }
    function main() i32 { return 0; }
  )"),
                                "duplicate lifetime parameter");
}

// A method may not use a lifetime its class did not declare.
TEST(Lambdas_LifetimeSyntax, undeclared_class_lifetime_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class C {
        var x: i32;
        init() { this.x = 0; }
        public method m(g: <'b>() -> i32) i32 { return g(); }
    }
    function main() i32 { return 0; }
  )"),
                                "use of undeclared lifetime 'b");
}

// Lifetimes come before type parameters, as in Rust.
TEST(Lambdas_LifetimeSyntax, lifetime_after_type_param_rejected) {
  EXPECT_THROW(executeString(R"(
    function f<T, 'a>(x: T) T { return x; }
    function main() i32 { return 0; }
  )"),
               SunError);
}

// The retired `[ref]` spelling points at the current one.
TEST(Lambdas_LifetimeSyntax, bracket_lifetime_spelling_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function take(f: [ref](i32) -> i32, x: i32) i32 {
        return f(x);
    }
    function main() i32 { return 0; }
  )"),
                                "replaced by lifetime annotations");
}

// A '<'a>' marker straight after a generic argument list lexes as '<<';
// the parser splits it, exactly as it splits '>>' when lists close.
TEST(Lambdas_LifetimeSyntax, named_lambda_type_as_generic_argument) {
  auto value = executeString(R"(
    class Box<T> {
        var f: T;
        init(f: T) { this.f = f; }
        public method call() i32 { var g = this.f; return g(); }
    }
    function stash<'a>(f: <'a>() -> i32) i32 {
        var b = Box<<'a>() -> i32>(f);
        return b.call();
    }
    function main() i32 {
        var x = 42;
        return stash(lambda [ref x]() i32 { return x; });
    }
  )");
  EXPECT_EQ(value, 42);
}

// A multi-character body between apostrophes is still a character-literal
// diagnostic, not a lifetime.
TEST(Lambdas_LifetimeSyntax, multi_char_literal_still_diagnosed) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var c: char = 'ab';
        return 0;
    }
  )"),
               SunError);
}
