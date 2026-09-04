// tests/memory_safety/refs/test_holder_returns.cpp - Returning a value that
// stores references
//
// A class with a ref field is a holder: it borrows what it points at. A
// holder may leave the frame that built it only when everything it borrows
// outlives that frame - `this`, ref parameters, globals - and the caller then
// holds the holder's loans on the call's by-ref inputs, exactly as it would
// for a returned `ref`. Anything that points into the frame stays in it.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {

const char* kHolderReturn = "cannot return a value that stores references";

// A counter and a guard that bumps it when dropped: the shape of a lock
// guard, observable without threads.
const char* kCounterAndBump = R"(
    class Counter {
        var n: i32;
        init() { this.n = 0; }
        public method guard() Bump { return Bump(this); }
        public method guard_via_local() Bump { var g = Bump(this); return g; }
    }
    class Bump {
        var target: ref Counter;
        init(target: ref Counter) { this.target = target; }
        deinit() { this.target.n = this.target.n + 1; }
    }
)";

}  // namespace

// A method may hand back a holder of `this`: the receiver outlives the call,
// and the guard's deinit runs when the caller's scope ends.
TEST(MemorySafety_HolderReturns, method_returns_holder_of_this) {
  auto value = executeString(std::string(kCounterAndBump) + R"(
    function main() i32 {
        var c = Counter();
        if (true) {
            var g = c.guard();
        }
        if (true) {
            var g = c.guard();
        }
        return c.n;
    }
  )");
  EXPECT_EQ(value, 2);
}

// The same through a local the holder was parked in first.
TEST(MemorySafety_HolderReturns, method_returns_holder_local_of_this) {
  auto value = executeString(std::string(kCounterAndBump) + R"(
    function main() i32 {
        var c = Counter();
        if (true) {
            var g = c.guard_via_local();
        }
        return c.n;
    }
  )");
  EXPECT_EQ(value, 1);
}

// A free function may hand back a holder of a ref parameter.
TEST(MemorySafety_HolderReturns, function_returns_holder_of_ref_param) {
  auto value = executeString(std::string(kCounterAndBump) + R"(
    function make(c: ref Counter) Bump { return Bump(c); }
    function main() i32 {
        var c = Counter();
        if (true) {
            var g = make(c);
        }
        return c.n;
    }
  )");
  EXPECT_EQ(value, 1);
}

// A holder of a local dies with the frame, whether built directly or handed
// back by a call on that local.
TEST(MemorySafety_HolderReturns, holder_of_local_via_call_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounterAndBump) + R"(
    function make() Bump {
        var c = Counter();
        return c.guard();
    }
    function main() i32 { return 0; }
  )"),
      kHolderReturn);
}

// A by-value parameter is storage the frame owns, so a holder of it cannot
// leave either.
TEST(MemorySafety_HolderReturns, holder_of_by_value_param_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounterAndBump) + R"(
    function make(c: Counter) Bump { return c.guard(); }
    function main() i32 { return 0; }
  )"),
      kHolderReturn);
}

// A local holder that borrows a local carries that bound to the return.
TEST(MemorySafety_HolderReturns, local_holder_of_local_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounterAndBump) + R"(
    function make() Bump {
        var c = Counter();
        var g = Bump(c);
        return g;
    }
    function main() i32 { return 0; }
  )"),
      kHolderReturn);
}

// At the call site the returned holder borrows the receiver, so the receiver
// cannot be replaced while the holder lives.
TEST(MemorySafety_HolderReturns, returned_holder_keeps_receiver_borrowed) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounterAndBump) + R"(
    function main() i32 {
        var c = Counter();
        var g = c.guard();
        c = Counter();
        return 0;
    }
  )"),
      "Borrow check failed");
}

// And a holder handed back by a call cannot land in a name declared outside
// the scope of what it borrows.
TEST(MemorySafety_HolderReturns, returned_holder_cannot_outlive_receiver) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounterAndBump) + R"(
    function main() i32 {
        var outer = Counter();
        var g = outer.guard();
        if (true) {
            var inner = Counter();
            g = inner.guard();
        }
        return 0;
    }
  )"),
      "cannot store this value in 'g'");
}
