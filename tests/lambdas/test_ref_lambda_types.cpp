// tests/lambdas/test_ref_lambda_types.cpp - The '<'_>' lambda type marker
//
// A plain '(i32) -> i32' annotation is reserved for environment-free
// lambdas; '<'_>(i32) -> i32' admits lambdas that carry a captured
// environment living in a stack frame (capture lists, bound methods).
// Covers: assignability in both directions, the return-position and global
// bans, transitivity through fields and container instantiations, and the
// escape routes the marker closes.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// ============================================================================
// Assignability: clean widens into <'_>, never the reverse
// ============================================================================

// A capture-list lambda no longer fits a plain lambda annotation: the
// annotation promises an environment-free value the callee may keep.
TEST(Lambdas_RefLambdaTypes, capturing_lambda_rejected_by_clean_param) {
  EXPECT_THROW(executeString(R"(
    function apply(f: () -> i32) i32 {
        return f();
    }
    function main() i32 {
        var x = 3;
        return apply(lambda [ref x]() i32 { return x; });
    }
  )"),
               SunError);
}

// The identity launder from the design discussion: without the marker this
// compiled and the returned lambda read a dead frame.
TEST(Lambdas_RefLambdaTypes,
     capturing_lambda_cannot_launder_through_clean_param) {
  EXPECT_THROW(executeString(R"(
    function launder(f: () -> i32) () -> i32 {
        return f;
    }
    function main() i32 {
        var x = 3;
        var f = launder(lambda [ref x]() i32 { return x; });
        return f();
    }
  )"),
               SunError);
}

// A bound method holds its receiver by reference, so it is a '<'_>' value
// and a clean annotation rejects it too.
TEST(Lambdas_RefLambdaTypes, bound_method_rejected_by_clean_annotation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Counter {
        var count: i32;
        init() { this.count = 0; }
        method add(x: i32) i32 {
            this.count = this.count + x;
            return this.count;
        }
    }
    function main() i32 {
        var c = Counter();
        var f: (i32) -> i32 = c.add;
        return f(1);
    }
  )"),
                                "Cannot assign value of type");
}

// An owned capture pins the environment to the frame just as a borrow does,
// so a '[x]' lambda is a '<'_>' value as well.
TEST(Lambdas_RefLambdaTypes,
     owned_capture_lambda_rejected_by_clean_annotation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
        var x = 3;
        var f: () -> i32 = lambda [x]() i32 { return x; };
        return f();
    }
  )"),
                                "Cannot assign value of type");
}

// The widening direction: an environment-free lambda goes anywhere a
// '<'_>' one is accepted.
TEST(Lambdas_RefLambdaTypes, clean_lambda_widens_into_ref_param) {
  auto value = executeString(R"(
    function apply(f: <'_>(i32) -> i32, x: i32) i32 {
        return f(x);
    }
    function main() i32 {
        var f: <'_>() -> i32 = lambda () i32 { return 40; };
        return f() + apply(lambda (n: i32) i32 { return n; }, 2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A '<'_>' param takes capture-list lambdas and bound methods, and the
// callee may call them - the whole point of the marker.
TEST(Lambdas_RefLambdaTypes, ref_param_accepts_and_calls_capturing_values) {
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        init() { this.count = 0; }
        method add(x: i32) i32 {
            this.count = this.count + x;
            return this.count;
        }
    }
    function apply(f: <'_>(i32) -> i32, x: i32) i32 {
        return f(x);
    }
    function main() i32 {
        var c = Counter();
        var base = 30;
        apply(c.add, 5);
        var viaCapture = apply(lambda [ref base](n: i32) i32 { return base + n; }, 2);
        return viaCapture + c.count + 2;
    }
  )");
  EXPECT_EQ(value, 39);
}

// The throws marker and the <'_> marker widen independently: a
// non-throwing capture-list lambda fits a throwing '<'_>' parameter.
TEST(Lambdas_RefLambdaTypes, ref_and_throws_widen_together) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function run_guarded(f: <'_>(i32) -> i32 throws IError, x: i32) i32 {
        try {
            return f(x);
        } catch (e: IError) {
            return -1;
        }
    }
    function main() i32 {
        var k = 40;
        return run_guarded(lambda [ref k](n: i32) i32 { return k + n; }, 2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Positional bans: no <'_> return types, no frame-carrying globals
// ============================================================================

TEST(Lambdas_RefLambdaTypes, ref_lambda_type_banned_in_return_position) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function make() <'_>() -> i32 {
        var x = 3;
        return lambda [ref x]() i32 { return x; };
    }
    function main() i32 {
        return 0;
    }
  )"),
                                "cannot be a return type");
}

TEST(Lambdas_RefLambdaTypes, ref_lambda_type_banned_in_nested_return_position) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function take(f: () -> <'_>() -> i32) i32 {
        return 0;
    }
    function main() i32 {
        return 0;
    }
  )"),
                                "cannot be a return type");
}

// A global outlives every frame, so a '<'_>' type cannot be its type even
// when the value stored today happens to be environment-free.
TEST(Lambdas_RefLambdaTypes, global_cannot_have_ref_lambda_type) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    var g: <'_>() -> i32 = lambda () i32 { return 0; };
    function main() i32 {
        return 0;
    }
  )"),
                                "frame-carrying type");
}

// ============================================================================
// Transitivity: a class or container holding a <'_> lambda is frame-bound
// ============================================================================

// A '<'_>' field is legal, and the object works normally inside the frame.
TEST(Lambdas_RefLambdaTypes, class_with_ref_field_works_in_frame) {
  auto value = executeString(R"(
    class Holder {
        var f: <'_>() -> i32;
        init(f: <'this>() -> i32) { this.f = f; }
        method call() i32 { var g = this.f; return g(); }
    }
    function main() i32 {
        var x = 42;
        var h = Holder(lambda [ref x]() i32 { return x; });
        return h.call();
    }
  )");
  EXPECT_EQ(value, 42);
}

// But the object is frame-carrying: returning it would carry the lambda's
// environment out of the frame it lives in.
TEST(Lambdas_RefLambdaTypes, class_with_ref_field_cannot_be_returned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Holder {
        var f: <'_>() -> i32;
        init(f: <'this>() -> i32) { this.f = f; }
    }
    function make() Holder {
        var x = 3;
        return Holder(lambda [ref x]() i32 { return x; });
    }
    function main() i32 {
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// Nor may it hide behind an interface: the conversion would erase the
// frame binding from the type.
TEST(Lambdas_RefLambdaTypes, frame_carrying_class_cannot_become_interface) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface ICallable {
        method call() i32;
    }
    class Holder {
        var f: <'_>() -> i32;
        init(f: <'this>() -> i32) { this.f = f; }
        public method call() i32 { var g = this.f; return g(); }
    }
    function main() i32 {
        var x = 3;
        var h = Holder(lambda [ref x]() i32 { return x; });
        var i: ICallable = h;
        return i.call();
    }
  )"),
                                "Cannot assign value of type");
}

// A global of a frame-carrying class type is banned like the bare lambda.
TEST(Lambdas_RefLambdaTypes, global_cannot_have_frame_carrying_class_type) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Holder {
        var f: <'_>() -> i32;
        init() { this.f = lambda () i32 { return 0; }; }
    }
    var g = Holder();
    function main() i32 {
        return 0;
    }
  )"),
                                "frame-carrying type");
}

// A generic class instantiated over a '<'_>' lambda type works inside the
// frame - the local pool of borrowing callbacks the marker exists for.
// (Vec cannot hold lambdas yet: enum payloads do not support lambda types,
// and Vec's API mentions Option<T>.)
TEST(Lambdas_RefLambdaTypes, generic_class_over_ref_lambda_works_in_frame) {
  auto value = executeString(R"(
    class Box<T> {
        var f: T;
        init(f: T) { this.f = f; }
        public method call() i32 { var g = this.f; return g(); }
    }
    function main() i32 {
        var a = 40;
        var b = Box<<'_>() -> i32>(lambda [ref a]() i32 { return a + 2; });
        return b.call();
    }
  )");
  EXPECT_EQ(value, 42);
}

// A named callback cannot cross an erased generic setter into an unrelated
// receiver; otherwise Box could invoke it after its captured frame dies.
TEST(Lambdas_RefLambdaTypes,
     generic_set_rejects_callback_from_dead_frame) {
  EXPECT_THROW(executeString(R"(
    class Box<T> {
        var f: T;
        init(f: T) { this.f = f; }
        public method set(f: T) void { this.f = f; return; }
        public method call() i32 {
            var callback = this.f;
            return callback();
        }
    }
    function store<'a>(f: <'a>() -> i32,
                       box: ref Box<<'_>() -> i32>) void {
        box.set(f);
        return;
    }
    function main() i32 {
        var box = Box<<'_>() -> i32>(lambda () i32 { return 0; });
        if (true) {
            var dead = 42;
            store(lambda [ref dead]() i32 { return dead; }, box);
        }
        return box.call();
    }
  )"),
               SunError);
}

// Matching the erased callback lifetime to the generic receiver keeps the
// conservative generic check precise enough to accept a safe store.
TEST(Lambdas_RefLambdaTypes,
     generic_set_accepts_callback_tied_to_receiver) {
  auto value = executeString(R"(
    class Box<T> {
        var f: T;
        init(f: T) { this.f = f; }
        public method set(f: T) void { this.f = f; return; }
        public method call() i32 {
            var callback = this.f;
            return callback();
        }
    }
    function store<'a>(f: <'a>() -> i32,
                       box: ref 'a Box<<'_>() -> i32>) void {
        box.set(f);
        return;
    }
    function main() i32 {
        var kept = 42;
        var box = Box<<'_>() -> i32>(lambda () i32 { return 0; });
        store(lambda [ref kept]() i32 { return kept; }, box);
        return box.call();
    }
  )");
  EXPECT_EQ(value, 42);
}

// ...but the instantiation is frame-carrying through its type argument -
// the storage may be hidden behind raw memory, so the argument itself
// counts - and cannot leave the frame.
TEST(Lambdas_RefLambdaTypes, generic_class_over_ref_lambda_cannot_be_returned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Box<T> {
        var f: T;
        init(f: T) { this.f = f; }
    }
    function make() Box<<'_>() -> i32> {
        var x = 3;
        return Box<<'_>() -> i32>(lambda [ref x]() i32 { return x; });
    }
    function main() i32 {
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// ============================================================================
// Frame-sourced lambdas must not flow into destinations that outlive the
// frame - the escape-up routes
// ============================================================================

// A callee must not store a lambda capturing ITS OWN locals into an object
// reached through a ref parameter: the caller's object would outlive the
// callee's frame. (Verified as a dead-frame read before the rule existed.)
TEST(Lambdas_RefLambdaTypes, local_capture_cannot_enter_ref_param_object) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Bus {
        var cb: <'_>() -> i32;
        init() { this.cb = lambda () i32 { return 0; }; }
        public method subscribe(cb: <'this>() -> i32) void { this.cb = cb; return; }
    }
    function evil(bus: ref Bus) void {
        var x = 42;
        bus.subscribe(lambda [ref x]() i32 { return x; });
        return;
    }
    function main() i32 {
        var bus = Bus();
        evil(bus);
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// The same route with a bound method of a callee-local receiver.
TEST(Lambdas_RefLambdaTypes, local_bound_method_cannot_enter_ref_param_object) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Bus {
        var cb: <'_>() -> i32;
        init() { this.cb = lambda () i32 { return 0; }; }
        public method subscribe(cb: <'this>() -> i32) void { this.cb = cb; return; }
    }
    class Node {
        var v: i32;
        init() { this.v = 1; }
        public method onMsg() i32 { return this.v; }
    }
    function evil(bus: ref Bus) void {
        var n = Node();
        bus.subscribe(n.onMsg);
        return;
    }
    function main() i32 {
        var bus = Bus();
        evil(bus);
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// A method must not park a lambda over its own locals in a field of `this`:
// the object outlives the call.
TEST(Lambdas_RefLambdaTypes, method_local_capture_cannot_enter_this_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Bus {
        var cb: <'_>() -> i32;
        init() { this.cb = lambda () i32 { return 0; }; }
        public method arm() void {
            var x = 3;
            this.cb = lambda [ref x]() i32 { return x; };
            return;
        }
    }
    function main() i32 {
        var bus = Bus();
        bus.arm();
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// The legitimate wiring stays legal: a frame-local bus takes a frame-local
// handler's bound method, and publishing dispatches to the object.
TEST(Lambdas_RefLambdaTypes, local_bus_subscribes_local_bound_method) {
  auto value = executeString(R"(
    class Bus {
        var cb: <'_>(i32) -> i32;
        init() { this.cb = lambda (x: i32) i32 { return x; }; }
        public method subscribe(cb: <'this>(i32) -> i32) void { this.cb = cb; return; }
        public method publish(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Handler {
        var seen: i32;
        init() { this.seen = 0; }
        public method onMessage(x: i32) i32 {
            this.seen = this.seen + x;
            return this.seen;
        }
    }
    function main() i32 {
        var h = Handler();
        var bus = Bus();
        bus.subscribe(h.onMessage);
        bus.publish(20);
        bus.publish(22);
        return h.seen;
    }
  )");
  EXPECT_EQ(value, 42);
}

// So does an object subscribing its own method through a ref parameter:
// `this` outlives the whole frame, so `this.onMsg` is not frame-sourced.
TEST(Lambdas_RefLambdaTypes, object_subscribes_its_own_method_via_ref_param) {
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

// Once a frame-local object holds a frame-sourced lambda it is frame-bound
// itself: it cannot cross a call boundary by value and smuggle the lambda
// onward.
TEST(Lambdas_RefLambdaTypes,
     carrier_of_frame_sourced_lambda_cannot_pass_by_value) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Bus {
        var cb: <'_>() -> i32;
        init() { this.cb = lambda () i32 { return 0; }; }
        public method subscribe(cb: <'this>() -> i32) void { this.cb = cb; return; }
    }
    function consume(b: Bus) void { return; }
    function main() i32 {
        var x = 3;
        var bus = Bus();
        bus.subscribe(lambda [ref x]() i32 { return x; });
        consume(bus);
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// The clean and <'_> instantiations of one generic are distinct types and
// coexist in one program.
TEST(Lambdas_RefLambdaTypes, clean_and_ref_instantiations_coexist) {
  auto value = executeString(R"(
    class Box<T> {
        var f: T;
        init(f: T) { this.f = f; }
        public method call() i32 { var g = this.f; return g(); }
    }
    function main() i32 {
        var a = 20;
        var bound = Box<<'_>() -> i32>(lambda [ref a]() i32 { return a + 2; });
        var clean = Box<() -> i32>(lambda () i32 { return 20; });
        return bound.call() + clean.call();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Lambdas_RefLambdaTypes, callback_must_outlive_box) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Box {
        var f: <'_>() -> i32;

        init(f: <'this>() -> i32) {
            this.f = f;
        }

        public method set(f: <'this>() -> i32) void {
            this.f = f;
            return;
        }

        public method call() i32 {
            var callback = this.f;
            return callback();
        }
    }

    function store<'a>(
        f: <'a>() -> i32,
        box: ref 'a Box
    ) void {
        box.set(f);
        return;
    }

    function main() i32 {
        var box = Box(lambda () i32 { return 0; });

        if (true) {
            var dead = 42;
            store(lambda [ref dead]() i32 { return dead; }, box);
            // Rejected: `dead` dies before `box`.
        }

        return box.call();
    }
  )"),
                                "Borrow check failed");
}
