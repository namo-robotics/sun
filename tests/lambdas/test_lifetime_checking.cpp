// tests/lambdas/test_lifetime_checking.cpp - Named-lifetime enforcement
//
// A lifetime name shared between two positions of one signature relates
// their arguments at every call site: '<'a>' parameters contribute
// captured environments, 'ref 'a T' parameters and class applications
// ('ref Bus<'this>') contribute the storage the callee may write into,
// and every environment must provably outlive every destination sharing
// its name. This closes issue #178's cross-function hole: the
// entanglement inside a callee is visible at its call sites as the
// shared name. Also covers the '<'a>' return unlock.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {

// One registry class used throughout: subscribe demands its callback
// outlive the receiver via the builtin 'this.
constexpr const char* kHolder = R"(
    class Holder {
        var cb: <'_>(i32) => i32;
        init() { this.cb = (x: i32) => i32 { return x; }; }
        public method set(cb: <'this>(i32) => i32) void { this.cb = cb; return; }
        public method call(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var v: i32;
        init(v: i32) { this.v = v; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
)";

}  // namespace

// ============================================================================
// The builtin 'this: a method parameter tied to its receiver
// ============================================================================

// An inner-scoped handler cannot enter an outer receiver.
TEST(Lambdas_LifetimeChecking, this_param_rejects_inner_scoped_handler) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(std::string(kHolder) + R"(
    function main() i32 {
        var h = Holder();
        if (true) {
            var n = Node(1);
            h.set(n.onMsg);
        }
        return h.call(1);
    }
  )"),
                                "Borrow check failed");
}

// Same scope satisfies the tie.
TEST(Lambdas_LifetimeChecking, this_param_accepts_same_scope_handler) {
  auto value = executeString(std::string(kHolder) + R"(
    function main() i32 {
        var h = Holder();
        var n = Node(40);
        h.set(n.onMsg);
        return h.call(2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Cross-function entanglement (issue #178, hole 2) at function level
// ============================================================================

// wire's signature says cb may end up inside dst; the call site checks the
// concrete scopes.
TEST(Lambdas_LifetimeChecking, wire_rejects_inner_source_outer_dest) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(std::string(kHolder) + R"(
    function wire<'a>(cb: <'a>(i32) => i32, dst: ref 'a Holder) void {
        dst.set(cb);
        return;
    }
    function main() i32 {
        var h = Holder();
        if (true) {
            var n = Node(1);
            wire(n.onMsg, h);
        }
        return h.call(1);
    }
  )"),
                                "Borrow check failed");
}

// The same wiring with compatible scopes runs.
TEST(Lambdas_LifetimeChecking, wire_accepts_outliving_source) {
  auto value = executeString(std::string(kHolder) + R"(
    function wire<'a>(cb: <'a>(i32) => i32, dst: ref 'a Holder) void {
        dst.set(cb);
        return;
    }
    function main() i32 {
        var n = Node(40);
        var h = Holder();
        if (true) {
            wire(n.onMsg, h);
        }
        return h.call(2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A callee cannot launder one lifetime into another: unrelated names do
// not satisfy each other.
TEST(Lambdas_LifetimeChecking, cross_name_store_rejected_in_callee) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(std::string(kHolder) + R"(
    function evil<'a, 'b>(cb: <'a>(i32) => i32, dst: ref 'b Holder) void {
        dst.set(cb);
        return;
    }
    function main() i32 { return 0; }
  )"),
                                "Borrow check failed");
}

// ============================================================================
// Cross-function entanglement at class level: the issue's attach example
// ============================================================================

// The full worked example: Bus<'a> stores 'a-bound callbacks; attach binds
// the bus's slot to the node's lifetime with ref Bus<'this>.
TEST(Lambdas_LifetimeChecking, attach_rejects_inner_node_outer_bus) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Bus<'a> {
        var cb: <'a>(i32) => i32;
        init() { this.cb = (x: i32) => i32 { return x; }; }
        public method subscribe(cb: <'a>(i32) => i32) void { this.cb = cb; return; }
        public method publish(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var total: i32;
        init() { this.total = 0; }
        public method onMsg(x: i32) i32 { this.total = this.total + x; return this.total; }
        public method attach(bus: ref Bus<'this>) void { bus.subscribe(this.onMsg); return; }
    }
    function main() i32 {
        var bus = Bus();
        if (true) {
            var n = Node();
            n.attach(bus);
        }
        return bus.publish(1);
    }
  )"),
                                "Borrow check failed");
}

// attach itself stays legal, both directly and through a helper whose
// arguments all come from ancestor frames.
TEST(Lambdas_LifetimeChecking, attach_accepts_compatible_scopes) {
  auto value = executeString(R"(
    class Bus<'a> {
        var cb: <'a>(i32) => i32;
        init() { this.cb = (x: i32) => i32 { return x; }; }
        public method subscribe(cb: <'a>(i32) => i32) void { this.cb = cb; return; }
        public method publish(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var total: i32;
        init() { this.total = 0; }
        public method onMsg(x: i32) i32 { this.total = this.total + x; return this.total; }
        public method attach(bus: ref Bus<'this>) void { bus.subscribe(this.onMsg); return; }
    }
    function wireUp<'a>(n: ref 'a Node, bus: ref Bus<'a>) void {
        n.attach(bus);
        return;
    }
    function main() i32 {
        var n = Node();
        var bus = Bus();
        wireUp(n, bus);
        bus.publish(40);
        bus.publish(2);
        return n.total;
    }
  )");
  EXPECT_EQ(value, 42);
}

// A method may store a class-lifetime parameter into its own fields - the
// class's contract covers it - but an unrelated function lifetime may not
// enter `this`.
TEST(Lambdas_LifetimeChecking, method_lifetime_cannot_enter_this_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Keeper {
        var cb: <'_>() => i32;
        init() { this.cb = () => i32 { return 0; }; }
        public method keep<'a>(cb: <'a>() => i32) void { this.cb = cb; return; }
    }
    function main() i32 { return 0; }
  )"),
                                "Borrow check failed");
}

// ============================================================================
// The '<'a>' return unlock
// ============================================================================

// pick chooses between two frame-bound lambdas; the caller can use the
// result inside the frame.
TEST(Lambdas_LifetimeChecking, named_return_selects_and_runs) {
  auto value = executeString(R"(
    function pick<'a>(x: <'a>() => i32, y: <'a>() => i32, first: bool) <'a>() => i32 {
        if (first) { return x; }
        return y;
    }
    function main() i32 {
        var a = 40;
        var b = 2;
        var f = pick([const ref a]() => i32 { return a; },
                     [const ref b]() => i32 { return b; }, false);
        var g = pick([const ref a]() => i32 { return a; },
                     [const ref b]() => i32 { return b; }, true);
        return f() + g();
    }
  )");
  EXPECT_EQ(value, 42);
}

// The result stays pinned to the arguments' scopes: storing it in an
// outer variable is the sub-frame escape again.
TEST(Lambdas_LifetimeChecking, named_return_result_cannot_escape_scope) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function pick<'a>(x: <'a>() => i32, y: <'a>() => i32, first: bool) <'a>() => i32 {
        if (first) { return x; }
        return y;
    }
    function main() i32 {
        var f: <'_>() => i32 = () => i32 { return 0; };
        if (true) {
            var a = 40;
            var b = 2;
            f = pick([const ref a]() => i32 { return a; },
                     [const ref b]() => i32 { return b; }, true);
        }
        return f();
    }
  )"),
                                "Borrow check failed");
}

// A function must actually honor its named return: returning a lambda
// over its own locals is still the classic escape.
TEST(Lambdas_LifetimeChecking, named_return_rejects_local_capture) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function bad<'a>(x: <'a>() => i32) <'a>() => i32 {
        var z = 7;
        return [ref z]() => i32 { return z; };
    }
    function main() i32 { return 0; }
  )"),
                                "Borrow check failed");
}

// A bare '<'_>' return stays banned; the error suggests naming the frame.
TEST(Lambdas_LifetimeChecking, bare_ref_return_still_banned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function make() <'_>() => i32 {
        return () => i32 { return 0; };
    }
    function main() i32 { return 0; }
  )"),
                                "Name the frame with a lifetime");
}

// ============================================================================
// Lambda literal lifetime binders
// ============================================================================

// A lambda's own lifetime relationship is enforced at every invocation.
TEST(Lambdas_LifetimeChecking,
     lambda_lifetime_rejects_inner_source_for_outer_destination) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Box {
        var f: <'this>() => i32;
        init() { this.f = () => i32 { return 0; }; }
        public method set(f: <'this>() => i32) void {
            this.f = f;
            return;
        }
    }
    function main() i32 {
        var box = Box();
        var store = <'a>(
            f: <'a>() => i32,
            destination: ref 'a Box
        ) => void {
            destination.set(f);
            return;
        };
        if (true) {
            var dead = 42;
            store([ref dead]() => i32 { return dead; }, box);
        }
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// ============================================================================
// Class journeys and 'this fields
// ============================================================================

// A '<'this>' field makes a class self-bound with no declared
// parameter: whatever it stores must outlive the object.
TEST(Lambdas_LifetimeChecking, this_field_class_stores_and_fires) {
  auto value = executeString(R"(
    class Keeper {
        var cb: <'this>(i32) => i32;
        init() { this.cb = (x: i32) => i32 { return x; }; }
        public method keep(cb: <'this>(i32) => i32) void { this.cb = cb; return; }
        public method fire(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var v: i32;
        init(v: i32) { this.v = v; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
    function main() i32 {
        var k = Keeper();
        var n = Node(40);
        k.keep(n.onMsg);
        return k.fire(2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A bus that received a handler moves between locals; the journey may not
// leave the handler's scope.
TEST(Lambdas_LifetimeChecking, carrier_journey_rejected_past_env_scope) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(std::string(kHolder) + R"(
    function main() i32 {
        var keep = Holder();
        if (true) {
            var n = Node(1);
            var h = Holder();
            h.set(n.onMsg);
            keep = h;
        }
        return keep.call(1);
    }
  )"),
                                "Borrow check failed");
}

// The same journey inside the handler's scope is fine.
TEST(Lambdas_LifetimeChecking, carrier_journey_within_env_scope_accepted) {
  auto value = executeString(std::string(kHolder) + R"(
    function main() i32 {
        var n = Node(40);
        var keep = Holder();
        if (true) {
            var h = Holder();
            h.set(n.onMsg);
            keep = h;
        }
        return keep.call(2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Interfaces carry the same contracts
// ============================================================================

// An interface method's 'this tie is enforced on implementers and at call
// sites against the implementing class.
TEST(Lambdas_LifetimeChecking, interface_this_param_enforced_at_call) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface ISink {
        public method accept(cb: <'this>(i32) => i32) void;
    }
    class Holder implements ISink {
        var cb: <'_>(i32) => i32;
        init() { this.cb = (x: i32) => i32 { return x; }; }
        public method accept(cb: <'this>(i32) => i32) void { this.cb = cb; return; }
    }
    class Node {
        var v: i32;
        init(v: i32) { this.v = v; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
    function main() i32 {
        var h = Holder();
        if (true) {
            var n = Node(1);
            h.accept(n.onMsg);
        }
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// Dropping the interface's lifetime tie in the implementation is a
// conformance error.
TEST(Lambdas_LifetimeChecking, interface_lifetime_conformance_required) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface ISink {
        public method accept(cb: <'this>(i32) => i32) void;
    }
    class Loose implements ISink {
        var x: i32;
        init() { this.x = 0; }
        public method accept(cb: <'_>(i32) => i32) void { return; }
    }
    function main() i32 { return 0; }
  )"),
                                "must match the interface's exactly");
}

// A generic class keeps its declared lifetimes in every specialization -
// lifetimes never key the specialization, but the contract survives it.
TEST(Lambdas_LifetimeChecking, generic_class_specialization_keeps_lifetimes) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Pair<'a, T> {
        var cb: <'a>(T) => T;
        var seed: T;
        init(seed: T) { this.seed = seed; this.cb = (x: T) => T { return x; }; }
        public method subscribe(cb: <'a>(T) => T) void { this.cb = cb; return; }
        public method fire() T { var f = this.cb; return f(this.seed); }
    }
    class Node {
        var v: i32;
        init(v: i32) { this.v = v; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
    function main() i32 {
        var p = Pair<i32>(2);
        if (true) {
            var n = Node(40);
            p.subscribe(n.onMsg);
        }
        return p.fire();
    }
  )"),
                                "Borrow check failed");
}

// And the compatible-scope instantiation runs.
TEST(Lambdas_LifetimeChecking, generic_class_with_lifetime_runs) {
  auto value = executeString(R"(
    class Pair<'a, T> {
        var cb: <'a>(T) => T;
        var seed: T;
        init(seed: T) { this.seed = seed; this.cb = (x: T) => T { return x; }; }
        public method subscribe(cb: <'a>(T) => T) void { this.cb = cb; return; }
        public method fire() T { var f = this.cb; return f(this.seed); }
    }
    class Node {
        var v: i32;
        init(v: i32) { this.v = v; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
    function main() i32 {
        var n = Node(40);
        var p = Pair<i32>(2);
        p.subscribe(n.onMsg);
        return p.fire();
    }
  )");
  EXPECT_EQ(value, 42);
}
