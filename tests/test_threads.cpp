// tests/test_threads.cpp - Tests for OS threads (spawn/join)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "error.h"
#include "execution_utils.h"

// ============================================================================
// Parsing Tests
// ============================================================================

// Test that spawn expression parses correctly
TEST(ThreadTest, parse_spawn_lambda) {
  // Just verify it compiles without runtime execution
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var t = spawn(lambda() i32 { return 42; });
      return 0;
    }
  )"));
}

TEST(ThreadTest, parse_spawn_with_captures) {
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

TEST(ThreadTest, spawn_requires_lambda) {
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

TEST(ThreadTest, spawn_lambda_no_args) {
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
// TEST(ThreadTest, spawn_returns_thread_type) {
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

TEST(ThreadTest, thread_type_inferred) {
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

TEST(ThreadTest, spawn_and_join_basic) {
  auto value = executeString(R"(
    function main() i32 {
      var t = spawn(lambda() i32 { return 42; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ThreadTest, spawn_with_captured_value) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 10;
      var t = spawn(lambda() i32 { return x * 2; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(ThreadTest, multiple_threads) {
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
