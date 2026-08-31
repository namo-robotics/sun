// tests/lambdas/test_sub_frame_scopes.cpp - Sub-frame scope tracking
//
// The frame rules make a '<'_>' lambda's environment safe at frame
// granularity; these tests cover the finer grain (issue #178): a value
// pinned to an inner scope must not be stored into a destination declared
// in an outer one, even though both live in the same frame. Covers the
// direct sub-frame store for lambdas (hole 1) and for ref-storing class
// values (hole 3's direct case), and the journeys that must stay legal.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// ============================================================================
// Hole 1: a destination in an outer scope outlives a source in an inner one
// ============================================================================

// The issue's repro: the bus outlives the node whose bound method it stores.
TEST(Lambdas_SubFrameScopes, outer_bus_rejects_inner_scoped_bound_method) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Bus {
        var cb: <'_>() -> i32;
        init() { this.cb = lambda () i32 { return 0; }; }
        public method subscribe(cb: <'this>() -> i32) void { this.cb = cb; return; }
        public method publish() i32 { var f = this.cb; return f(); }
    }
    class Node {
        var v: i32;
        init() { this.v = 1; }
        public method onMsg() i32 { return this.v; }
    }
    function main() i32 {
        var bus = Bus();
        if (true) {
            var n = Node();
            bus.subscribe(n.onMsg);
        }
        return bus.publish();
    }
  )"),
                                "Borrow check failed");
}

// The same hole with a capture-list literal over an inner-scoped variable.
TEST(Lambdas_SubFrameScopes, outer_bus_rejects_inner_scoped_capture) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Bus {
        var cb: <'_>() -> i32;
        init() { this.cb = lambda () i32 { return 0; }; }
        public method subscribe(cb: <'this>() -> i32) void { this.cb = cb; return; }
    }
    function main() i32 {
        var bus = Bus();
        if (true) {
            var x = 42;
            bus.subscribe(lambda [ref x]() i32 { return x; });
        }
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// Plain assignment is the same store without a call in between.
TEST(Lambdas_SubFrameScopes, outer_lambda_var_rejects_inner_scoped_capture) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
        var f: <'_>() -> i32 = lambda () i32 { return 0; };
        if (true) {
            var x = 3;
            f = lambda [ref x]() i32 { return x; };
        }
        return f();
    }
  )"),
                                "Borrow check failed");
}

// A loop body's locals die every iteration, so they are inner-scoped too.
TEST(Lambdas_SubFrameScopes, outer_bus_rejects_loop_local_bound_method) {
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
    function main() i32 {
        var bus = Bus();
        var i = 0;
        while (i < 1) {
            var n = Node();
            bus.subscribe(n.onMsg);
            i = i + 1;
        }
        return 0;
    }
  )"),
                                "Borrow check failed");
}

// ============================================================================
// What must stay legal: sources that outlive their destination
// ============================================================================

// Both names in the same scope - the wiring the feature exists for.
TEST(Lambdas_SubFrameScopes, same_scope_subscribe_inside_block_accepted) {
  auto value = executeString(R"(
    class Bus {
        var cb: <'_>(i32) -> i32;
        init() { this.cb = lambda (x: i32) i32 { return x; }; }
        public method subscribe(cb: <'this>(i32) -> i32) void { this.cb = cb; return; }
        public method publish(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var v: i32;
        init() { this.v = 40; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
    function main() i32 {
        var out = 0;
        if (true) {
            var bus = Bus();
            var n = Node();
            bus.subscribe(n.onMsg);
            out = bus.publish(2);
        }
        return out;
    }
  )");
  EXPECT_EQ(value, 42);
}

// An outer handler flowing into an inner bus: the source outlives the
// destination, so the store is fine.
TEST(Lambdas_SubFrameScopes, inner_bus_accepts_outer_scoped_handler) {
  auto value = executeString(R"(
    class Bus {
        var cb: <'_>(i32) -> i32;
        init() { this.cb = lambda (x: i32) i32 { return x; }; }
        public method subscribe(cb: <'this>(i32) -> i32) void { this.cb = cb; return; }
        public method publish(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var v: i32;
        init() { this.v = 40; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
    function main() i32 {
        var n = Node();
        var out = 0;
        if (true) {
            var bus = Bus();
            bus.subscribe(n.onMsg);
            out = bus.publish(2);
        }
        return out;
    }
  )");
  EXPECT_EQ(value, 42);
}

// The store's own position does not matter - only the declarations do. A
// subscribe that runs inside a branch still wires two outer names.
TEST(Lambdas_SubFrameScopes, branch_position_does_not_pin_outer_names) {
  auto value = executeString(R"(
    class Bus {
        var cb: <'_>(i32) -> i32;
        init() { this.cb = lambda (x: i32) i32 { return x; }; }
        public method subscribe(cb: <'this>(i32) -> i32) void { this.cb = cb; return; }
        public method publish(x: i32) i32 { var f = this.cb; return f(x); }
    }
    class Node {
        var v: i32;
        init() { this.v = 40; }
        public method onMsg(x: i32) i32 { return this.v + x; }
    }
    function main() i32 {
        var bus = Bus();
        var n = Node();
        if (true) {
            bus.subscribe(n.onMsg);
        }
        return bus.publish(2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// Same-scope reassignment of a '<'_>' local keeps working.
TEST(Lambdas_SubFrameScopes, outer_lambda_var_accepts_same_scope_capture) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 42;
        var f: <'_>() -> i32 = lambda () i32 { return 0; };
        f = lambda [ref x]() i32 { return x; };
        return f();
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Hole 3's direct case: a ref-storing class value must not land in a holder
// declared in an outer scope than what it borrows
// ============================================================================

// Reassigning an outer holder from an inner scope would keep the borrowed
// variable's address past its death.
TEST(Lambdas_SubFrameScopes, outer_holder_rejects_inner_scoped_borrow) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        init(v: i32) { this.v = v; }
    }
    class Holder {
        public var x: ref Inner;
        init(x: ref Inner) { this.x = x; }
    }
    function main() i32 {
        var a = Inner(1);
        var h = Holder(a);
        if (true) {
            var b = Inner(2);
            h = Holder(b);
        }
        return h.x.v;
    }
  )"),
                                "Borrow check failed");
}

// A holder local moved toward an outer name carries its borrow's scope
// along: the journey is rejected at the move that would outlive it.
TEST(Lambdas_SubFrameScopes, holder_moved_to_outer_name_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        init(v: i32) { this.v = v; }
    }
    class Holder {
        public var x: ref Inner;
        init(x: ref Inner) { this.x = x; }
    }
    function main() i32 {
        var a = Inner(1);
        var keep = Holder(a);
        if (true) {
            var b = Inner(2);
            var h = Holder(b);
            keep = h;
        }
        return keep.x.v;
    }
  )"),
                                "Borrow check failed");
}

// Same-scope reassignment stays legal: the new borrow outlives the holder.
TEST(Lambdas_SubFrameScopes, same_scope_holder_reassignment_accepted) {
  auto value = executeString(R"(
    class Inner {
        var v: i32;
        init(v: i32) { this.v = v; }
    }
    class Holder {
        public var x: ref Inner;
        init(x: ref Inner) { this.x = x; }
    }
    function main() i32 {
        var a = Inner(1);
        var b = Inner(42);
        var h = Holder(a);
        h = Holder(b);
        return h.x.v;
    }
  )");
  EXPECT_EQ(value, 42);
}

// A journey through an inner name back to a same-scope holder is fine: the
// borrowed variable outlives every owner the value ever has.
TEST(Lambdas_SubFrameScopes, holder_journey_within_borrow_scope_accepted) {
  auto value = executeString(R"(
    class Inner {
        var v: i32;
        init(v: i32) { this.v = v; }
    }
    class Holder {
        public var x: ref Inner;
        init(x: ref Inner) { this.x = x; }
    }
    function main() i32 {
        var a = Inner(1);
        var b = Inner(42);
        var keep = Holder(a);
        if (true) {
            var h = Holder(b);
            keep = h;
        }
        return keep.x.v;
    }
  )");
  EXPECT_EQ(value, 42);
}
