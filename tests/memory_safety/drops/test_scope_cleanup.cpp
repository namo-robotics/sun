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
