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
