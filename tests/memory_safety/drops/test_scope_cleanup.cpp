// Tests for block-scope drop correctness: owners (classes with deinit) must
// be dropped at the end of the scope they were declared in — if/else blocks,
// per loop iteration, on break/continue, and on returns that jump out of
// nested scopes.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {

// Shared preamble: a global counter and a class whose deinit increments it.
const char* kOwnerPreamble = R"(
    var counter: i32 = 0;

    class Owner {
      function init() {}
      function deinit() void {
        counter = counter + 1;
      }
    }
)";

std::string withPreamble(const std::string& body) {
  return std::string(kOwnerPreamble) + body;
}

}  // namespace

TEST(MemorySafety_Drops_ScopeCleanup, if_block_owner_dropped_at_block_exit) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      if (true) {
        var f = Owner();
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  // Dropped when the if-block ends, before helper returns
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_ScopeCleanup, else_block_owner_dropped_at_block_exit) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      if (false) {
        var f = Owner();
      } else {
        var g = Owner();
        var h = Owner();
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_ScopeCleanup, while_body_owner_dropped_per_iteration) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var i: i32 = 0;
      while (i < 3) {
        var f = Owner();
        i = i + 1;
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  // One drop per iteration, not one at loop end
  EXPECT_EQ(value, 3);
}

TEST(MemorySafety_Drops_ScopeCleanup, for_body_owner_dropped_per_iteration) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      for (var i: i32 = 0; i < 4; i = i + 1) {
        var f = Owner();
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  EXPECT_EQ(value, 4);
}

TEST(MemorySafety_Drops_ScopeCleanup, break_drops_body_owner) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      while (true) {
        var f = Owner();
        break;
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_ScopeCleanup,
     break_from_nested_if_drops_all_jumped_scopes) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      while (true) {
        var b = Owner();
        if (true) {
          var c = Owner();
          break;
        }
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  // break jumps out of the if scope and the loop body scope: both drop
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_ScopeCleanup,
     continue_drops_body_owner_each_iteration) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      for (var i: i32 = 0; i < 3; i = i + 1) {
        var f = Owner();
        continue;
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  EXPECT_EQ(value, 3);
}

TEST(MemorySafety_Drops_ScopeCleanup,
     return_from_nested_block_drops_outer_scopes) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var outer = Owner();
      if (true) {
        var inner = Owner();
        return 0;
      }
      return 1;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  // Return inside the if must drop both the if-scope owner and the
  // function-scope owner
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_ScopeCleanup, return_from_loop_drops_all_live_scopes) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var a = Owner();
      for (var i: i32 = 0; i < 5; i = i + 1) {
        var b = Owner();
        if (i == 2) {
          var c = Owner();
          return 0;
        }
      }
      return 1;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  // Iterations 0 and 1 drop b at the back-edge (2 drops); at i == 2 the
  // return drops c, b, and a (3 drops)
  EXPECT_EQ(value, 5);
}

TEST(MemorySafety_Drops_ScopeCleanup, explicit_deinit_in_block_no_double_drop) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      if (true) {
        var f = Owner();
        f.deinit();
      }
      return counter;
    }

    function main() i32 {
      return helper();
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_ScopeCleanup,
     loop_scoped_init_var_dropped_once_after_loop) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var before: i32 = 0;
      for (var i: i32 = 0; i < 3; i = i + 1) {
        before = counter;
      }
      return before;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  // No owners in this loop at all; sanity-check counter stays 0
  EXPECT_EQ(value, 0);
}

// -------------------------------------------------------------------
// A call result nobody takes
// -------------------------------------------------------------------
// A function returning a class by value hands the caller something the
// caller owns. `var x = f();` adopts it, and passing it straight on moves
// it, but a result that is simply discarded used to be materialized into an
// untracked slot and never dropped.

TEST(MemorySafety_Drops_ScopeCleanup, discarded_call_result_is_dropped) {
  auto value = executeString(withPreamble(R"(
    function make() Owner { return Owner(); }

    function helper() i32 {
      if (true) {
        make();
        make();
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 2);
}

// The taken and the discarded result must each be dropped exactly once —
// tracking the temporary must not double up with the variable that adopts it.
TEST(MemorySafety_Drops_ScopeCleanup, taken_and_discarded_results_drop_once) {
  auto value = executeString(withPreamble(R"(
    function make() Owner { return Owner(); }

    function helper() i32 {
      if (true) {
        var a = make();
        make();
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 2);
}

// A discarded method result is dropped the same way a free function's is.
TEST(MemorySafety_Drops_ScopeCleanup, discarded_method_result_is_dropped) {
  auto value = executeString(withPreamble(R"(
    class Factory {
      function init() {}
      function make() Owner { return Owner(); }
    }

    function helper() i32 {
      var f = Factory();
      if (true) {
        f.make();
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}

// -------------------------------------------------------------------
// By-value compound parameters
// -------------------------------------------------------------------
// Passing a class by value moves it, so the callee owns it from then on and
// is what drops it. A parameter the body passes on is marked moved and
// dropped wherever it landed instead.

TEST(MemorySafety_Drops_ScopeCleanup, by_value_param_is_dropped_by_the_callee) {
  auto value = executeString(withPreamble(R"(
    function ignore(o: Owner) void { }

    function helper() i32 {
      if (true) {
        ignore(Owner());
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}

// The parameter is read but not moved on, so the callee still drops it.
TEST(MemorySafety_Drops_ScopeCleanup, read_only_param_is_still_dropped) {
  auto value = executeString(withPreamble(R"(
    class Boxed {
      public var n: i32;
      public function init(n: i32) { this.n = n; }
      public function deinit() void { counter = counter + 1; }
    }
    function read(b: Boxed) i32 { return b.n; }

    function helper() i32 {
      if (true) {
        var n = read(Boxed(7));
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}

// Handed straight on: dropped once, at the end of the chain.
TEST(MemorySafety_Drops_ScopeCleanup, param_passed_on_is_dropped_once) {
  auto value = executeString(withPreamble(R"(
    function ignore(o: Owner) void { }
    function relay(o: Owner) void { ignore(o); }
    function relay2(o: Owner) void { relay(o); }

    function helper() i32 {
      if (true) {
        relay2(Owner());
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}

// Returned rather than dropped: the caller takes it over.
TEST(MemorySafety_Drops_ScopeCleanup, param_returned_is_dropped_by_the_caller) {
  auto value = executeString(withPreamble(R"(
    function give_back(o: Owner) Owner { return o; }

    function helper() i32 {
      var seen: i32 = 0;
      if (true) {
        var back = give_back(Owner());
        seen = counter;   // nothing dropped yet
      }
      return seen * 10 + counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);  // 0 while the caller holds it, 1 once it goes
}

// Stored into a field: the object owns it, and drops it when the object goes.
// Tagged rather than Owner because assigning a class-typed field drops what
// the field held first, and a fresh object's field is zeroed — so this needs
// the usual discipline of a deinit that is a no-op on the all-zero state.
TEST(MemorySafety_Drops_ScopeCleanup, param_stored_in_a_field_is_dropped_once) {
  auto value = executeString(withPreamble(R"(
    class Tagged {
      public var id: i32;
      public function init(id: i32) { this.id = id; }
      public function deinit() void {
        if (this.id != 0) { counter = counter + 1; this.id = 0; }
      }
    }
    class Holder {
      public var t: Tagged;
      public function init(t: Tagged) { this.t = t; }
    }

    function helper() i32 {
      if (true) {
        var h = Holder(Tagged(7));
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}

// A borrow is not an owner: only the caller's own value is dropped.
TEST(MemorySafety_Drops_ScopeCleanup, ref_param_is_not_dropped_by_the_callee) {
  auto value = executeString(withPreamble(R"(
    function borrow(o: ref Owner) void { }
    function look(o: const ref Owner) void { }

    function helper() i32 {
      if (true) {
        var a = Owner();
        borrow(a);
        look(a);
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}

// A lambda parameter is owned the same way a function's is.
TEST(MemorySafety_Drops_ScopeCleanup, by_value_lambda_param_is_dropped) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var take = lambda (o: Owner) i32 { return 0; };
      if (true) {
        var n = take(Owner());
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}

// A method's by-value parameter, and its receiver, which is a borrow.
TEST(MemorySafety_Drops_ScopeCleanup, by_value_method_param_is_dropped) {
  auto value = executeString(withPreamble(R"(
    class Sink {
      public function init() {}
      public function take(o: Owner) i32 { return 0; }
    }

    function helper() i32 {
      var s = Sink();
      if (true) {
        var n = s.take(Owner());
      }
      return counter;
    }

    function main() i32 { return helper(); }
  )"));
  EXPECT_EQ(value, 1);
}
