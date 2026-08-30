// tests/lambdas/test_capture_discovery.cpp - Tests that a lambda finds the
// variables its body uses, whatever kind of expression they are buried in.
// A name the free-variable walk misses is never captured, so the closure ends
// up looking for a local among the module's globals.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Expression shapes the walk has to look inside
// ============================================================================

TEST(Lambdas_CaptureDiscovery, scalar_used_inside_unsafe_block) {
  auto value = executeString(R"(
      function main() i32 {
          var n: i32 = 7;
          var f = lambda() i32 { unsafe { return n; }; };
          return f();
      }
    )");
  EXPECT_EQ(value, 7);
}

TEST(Lambdas_CaptureDiscovery, raw_pointer_used_inside_unsafe_block) {
  auto value = executeStringWithStdlib(R"(
      using sun;

      class Holder {
          public var counter: i64;
          init() { this.counter = 41; }
      }

      function main() i32 {
          var alloc = make_heap_allocator();
          var sp: raw_ptr<Holder> = alloc.create<Holder>();
          unsafe { sp.counter = 42; };
          var f = lambda() i64 { unsafe { return sp.counter; }; };
          var result = f();
          unsafe { _free(sp); };
          return _convert<i32>(result);
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(Lambdas_CaptureDiscovery, scalar_used_inside_try_block) {
  auto value = executeString(R"(
      function main() i32 {
          var n: i32 = 5;
          var f = lambda() i32 { try { return n; } catch (e: IError) { return 0; } };
          return f();
      }
    )");
  EXPECT_EQ(value, 5);
}

TEST(Lambdas_CaptureDiscovery, scalar_used_inside_catch_block) {
  auto value = executeStringWithStdlib(R"(
      using sun;

      function boom() i32 throws IError {
          throw Error(1, "boom");
      }

      function main() i32 {
          var n: i32 = 6;
          var f = lambda() i32 {
              try { return boom(); } catch (e: IError) { return n; }
          };
          return f();
      }
    )");
  EXPECT_EQ(value, 6);
}

TEST(Lambdas_CaptureDiscovery, scalar_used_inside_array_literal) {
  auto value = executeString(R"(
      function main() i32 {
          var n: i32 = 4;
          var f = lambda() i32 {
              var a = [n, n + 1, n + 2];
              return a[2];
          };
          return f();
      }
    )");
  EXPECT_EQ(value, 6);
}

TEST(Lambdas_CaptureDiscovery, scalar_used_in_a_thrown_error) {
  auto value = executeStringWithStdlib(R"(
      using sun;

      function main() i32 {
          var n: i32 = 8;
          var f = lambda() i32 throws IError { throw Error(n, "bad"); };
          try { return f(); } catch (e: IError) { return e.code(); }
      }
    )");
  EXPECT_EQ(value, 8);
}

TEST(Lambdas_CaptureDiscovery, scalar_used_in_a_string_interpolation) {
  auto value = executeStringWithStdlib(R"(
      using sun;

      function main() i32 {
          var n: i32 = 3;
          var f = lambda() i32 {
              var s = `n is ${n}`;
              return _convert<i32>(s.length());
          };
          return f();
      }
    )");
  EXPECT_EQ(value, 6);
}

// ============================================================================
// Names a construct declares are its own, not captures from outside
// ============================================================================

TEST(Lambdas_CaptureDiscovery, loop_counter_shadows_an_outer_name) {
  auto value = executeString(R"(
      function main() i32 {
          var i: i32 = 100;
          var f = lambda() i32 {
              var total: i32 = 0;
              for (var i: i32 = 0; i < 4; i += 1) { total += i; }
              return total;
          };
          return f() + i;
      }
    )");
  EXPECT_EQ(value, 106);
}

TEST(Lambdas_CaptureDiscovery, catch_binding_shadows_an_outer_name) {
  auto value = executeStringWithStdlib(R"(
      using sun;

      function boom() i32 throws IError {
          throw Error(9, "boom");
      }

      function main() i32 {
          var e: i32 = 100;
          var f = lambda() i32 {
              try { return boom(); } catch (e: IError) { return e.code(); }
          };
          return f() + e;
      }
    )");
  EXPECT_EQ(value, 109);
}

// `this` is not a name the discovery walk can find - it is its own node
// type - so it must be rejected outright: a lambda body compiles as a
// separate function and cannot reach the enclosing method's receiver.
// (Before this check, codegen died on an LLVM verifier error instead.)
TEST(Lambdas_CaptureDiscovery, lambda_cannot_use_this) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Counter {
          var count: i32;
          init() { this.count = 5; }
          method snapshot() i32 {
              var f = lambda () i32 { return this.count; };
              return f();
          }
      }
      function main() i32 {
          var c = Counter();
          return c.snapshot();
      }
    )"),
                                "A lambda cannot use 'this'");
}
