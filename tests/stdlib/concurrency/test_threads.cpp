// tests/stdlib/concurrency/test_threads.cpp - Tests for OS threads (spawn/join)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"
#include "support/error.h"

// ============================================================================
// Parsing Tests
// ============================================================================

// Test that spawn expression parses correctly
TEST(Stdlib_Concurrency_Threads, parse_spawn_lambda) {
  // Just verify it compiles without runtime execution
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var t = spawn(lambda() i32 { return 42; });
      return 0;
    }
  )"));
}

TEST(Stdlib_Concurrency_Threads, parse_spawn_with_captures) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 10;
      var t = spawn(lambda() i32 { return x + 1; });
      return 0;
    }
  )"));
}

// ============================================================================
// Semantic Analysis Tests
// ============================================================================

TEST(Stdlib_Concurrency_Threads, spawn_requires_lambda) {
  // spawn with non-lambda should fail semantic analysis
  EXPECT_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 42;
      var t = spawn(x);
      return 0;
    }
  )"),
               SunError);
}

TEST(Stdlib_Concurrency_Threads, spawn_lambda_no_args) {
  // spawn lambda must take no arguments
  EXPECT_THROW(compileString(R"(
    function main() i32 {
      var t = spawn(lambda(x: i32) i32 { return x; });
      return 0;
    }
  )"),
               SunError);
}

// Note: Thread<T> as a type annotation is not yet supported.
// The type is inferred from spawn() return value.
// This test is disabled until Thread<T> type annotation is implemented.
// TEST(Stdlib_Concurrency_Threads, spawn_returns_thread_type) {
//   EXPECT_NO_THROW(compileString(R"(
//     function main() i32 {
//       var t: Thread<i32> = spawn(lambda() i32 { return 42; });
//       return 0;
//     }
//   )"));
// }

// ============================================================================
// Type Inference Tests
// ============================================================================

TEST(Stdlib_Concurrency_Threads, thread_type_inferred) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var t = spawn(lambda() i64 { return 100; });
      // t should be inferred as Thread<i64>
      return 0;
    }
  )"));
}

// ============================================================================
// Runtime Tests (Basic)
// ============================================================================

TEST(Stdlib_Concurrency_Threads, spawn_and_join_basic) {
  auto value = executeString(R"(
    function main() i32 {
      var t = spawn(lambda() i32 { return 42; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Concurrency_Threads, spawn_with_captured_value) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 10;
      var t = spawn(lambda() i32 { return x * 2; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 20);
}

// Issue #99: a void lambda has no result slot; join returns nothing
TEST(Stdlib_Concurrency_Threads, spawn_void_lambda_and_join) {
  auto value = executeString(R"(
    function main() i32 {
      var t = spawn(lambda() void { });
      t.join();
      return 42;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Concurrency_Threads, spawn_void_lambda_without_join_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      spawn(lambda() void { });
      return 0;
    }
  )"));
}

// Issue #99: a lambda held in a variable arrives as a fat struct value
TEST(Stdlib_Concurrency_Threads, spawn_lambda_variable) {
  auto value = executeString(R"(
    function main() i32 {
      var f = lambda() i32 { return 7; };
      var t = spawn(f);
      return t.join() * 6;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Concurrency_Threads, spawn_lambda_variable_with_capture) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 10;
      var f = lambda() i32 { return x * 2; };
      var t = spawn(f);
      return t.join() + 1;
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(Stdlib_Concurrency_Threads, multiple_threads) {
  auto value = executeString(R"(
    function main() i32 {
      var t1 = spawn(lambda() i32 { return 10; });
      var t2 = spawn(lambda() i32 { return 20; });
      var t3 = spawn(lambda() i32 { return 30; });
      return t1.join() + t2.join() + t3.join();
    }
  )");
  EXPECT_EQ(value, 60);
}

// ============================================================================
// Scoped threads (issue #122): shared state through a by-ref capture
// ============================================================================

// The thread writes through the capture; the parent reads it after joining
TEST(Stdlib_Concurrency_Threads, byref_capture_shares_a_class) {
  auto value = executeString(R"(
    class Counter {
      public var n: i32;
      public function init() { this.n = 0; }
    }
    function main() i32 {
      var c = Counter();
      var t = spawn(lambda [ref c]() i32 {
        var i: i32 = 0;
        while (i < 1000) { c.n = c.n + 1; i = i + 1; }
        return 0;
      });
      var r = t.join();
      return c.n;
    }
  )");
  EXPECT_EQ(value, 1000);
}

// A thread nobody joined by hand is joined when its handle's scope ends, so
// its writes are complete and its captures were alive throughout
TEST(Stdlib_Concurrency_Threads, unjoined_thread_joins_at_scope_exit) {
  auto value = executeString(R"(
    class Counter {
      public var n: i32;
      public function init() { this.n = 0; }
    }
    function main() i32 {
      var c = Counter();
      if (true) {
        var t = spawn(lambda [ref c]() i32 {
          var i: i32 = 0;
          while (i < 100000) { c.n = c.n + 1; i = i + 1; }
          return 0;
        });
      }
      return c.n;
    }
  )");
  EXPECT_EQ(value, 100000);
}

// A return between spawn and join leaves the scope, so the join happens there
TEST(Stdlib_Concurrency_Threads, unjoined_thread_joins_on_early_return) {
  auto value = executeString(R"(
    class Counter {
      public var n: i32;
      public function init() { this.n = 0; }
    }
    function run() i32 {
      var c = Counter();
      var t = spawn(lambda [ref c]() i32 {
        var i: i32 = 0;
        while (i < 100000) { c.n = c.n + 1; i = i + 1; }
        return 0;
      });
      return 5;
    }
    function main() i32 {
      return run();
    }
  )");
  EXPECT_EQ(value, 5);
}

// Joining by hand leaves nothing for the scope exit to join
TEST(Stdlib_Concurrency_Threads, explicit_join_is_not_repeated_at_scope_exit) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 1;
      var t = spawn(lambda [ref x]() i32 { x = 2; return 40; });
      var r = t.join();
      return r + x;
    }
  )");
  EXPECT_EQ(value, 42);
}

// A handle is a value like any other: binding it to a second name moves it,
// so exactly one name still joins the thread
TEST(Stdlib_Concurrency_Threads, handle_moves_to_a_new_variable) {
  auto value = executeString(R"(
    function main() i32 {
      var t = spawn(lambda() i32 { return 11; });
      var t2 = t;
      return t2.join();
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Stdlib_Concurrency_Threads, moved_handle_cannot_be_joined) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
      var t = spawn(lambda() i32 { return 11; });
      var t2 = t;
      return t.join();
    }
  )"),
               SunError);
}

// Overwriting a handle joins the thread it held first
TEST(Stdlib_Concurrency_Threads, reassigned_handle_joins_previous_thread) {
  auto value = executeString(R"(
    function main() i32 {
      var t = spawn(lambda() i32 { return 1; });
      t = spawn(lambda() i32 { return 2; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 2);
}

// An exception unwinding out of the scope joins the thread on its way past
TEST(Stdlib_Concurrency_Threads, unjoined_thread_joins_while_unwinding) {
  auto value = executeString(R"(
    class Boom implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "boom"; }
    }
    class Counter {
      public var n: i32;
      public function init() { this.n = 0; }
    }
    function fail() i32, IError { throw Boom(); }
    function main() i32 {
      var c = Counter();
      try {
        var t = spawn(lambda [ref c]() i32 {
          var i: i32 = 0;
          while (i < 100000) { c.n = c.n + 1; i = i + 1; }
          return 0;
        });
        var r = fail();
      } catch (e: IError) {
        return c.n;
      };
      return 0;
    }
  )");
  EXPECT_EQ(value, 100000);
}

// A mutable capture is exclusive, as it is for any two lambdas: two threads
// cannot yet share one object they both write (issue #122 follow-up)
TEST(Stdlib_Concurrency_Threads, two_threads_cannot_capture_one_ref) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      var t1 = spawn(lambda [ref x]() i32 { x = 1; return 0; });
      var t2 = spawn(lambda [ref x]() i32 { x = 2; return 0; });
      return t1.join() + t2.join();
    }
  )"),
               SunError);
}

// A [const ref x] capture only reads, so it is a shared loan: any number of
// threads can hold one at once
TEST(Stdlib_Concurrency_Threads, two_threads_can_capture_one_const_ref) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      var t1 = spawn(lambda [const ref x]() i32 { return x + 1; });
      var t2 = spawn(lambda [const ref x]() i32 { return x + 2; });
      return t1.join() + t2.join();
    }
  )");
  EXPECT_EQ(value, 3);
}

// A class is shared read-only the same way
TEST(Stdlib_Concurrency_Threads, threads_share_a_class_by_const_ref) {
  auto value = executeString(R"(
    class Config {
      public var limit: i32;
      public function init(limit: i32) { this.limit = limit; }
    }
    function main() i32 {
      var cfg = Config(20);
      var t1 = spawn(lambda [const ref cfg]() i32 { return cfg.limit; });
      var t2 = spawn(lambda [const ref cfg]() i32 { return cfg.limit; });
      return t1.join() + t2.join();
    }
  )");
  EXPECT_EQ(value, 40);
}

// The capture is read-only even though the variable is not
TEST(Stdlib_Concurrency_Threads, const_ref_capture_cannot_be_written) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      var t = spawn(lambda [const ref x]() i32 { x = 1; return 0; });
      return t.join();
    }
  )"),
               SunError);
}

// A shared capture and a mutable one still conflict
TEST(Stdlib_Concurrency_Threads, const_ref_and_ref_captures_conflict) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      var t1 = spawn(lambda [ref x]() i32 { x = 1; return 0; });
      var t2 = spawn(lambda [const ref x]() i32 { return x; });
      return t1.join() + t2.join();
    }
  )"),
               SunError);
}