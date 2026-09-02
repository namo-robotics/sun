// tests/stdlib/concurrency/test_shared.cpp - Compile-time negatives for
// Shared<T>, the atomically reference-counted shared-ownership pointer.
// The runtime behavior tests (reference counting, locking, threads) live in
// stdlib/shared_tests.sun and run through `sun test`.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"
#include "support/error.h"

// A handle is a value like any other: binding it to a second name moves it,
// and the borrow checker rejects any later use of the old name.
TEST(Stdlib_Concurrency_Shared, moved_handle_cannot_be_used) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;
    class Res { public var v: i32; init(v: i32) { this.v = v; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Res>(alloc, Res(3));
      var s2 = s;
      return s.count();
    }
  )"),
               SunError);
}

// ============================================================================
// The value is reachable only through the lock
// ============================================================================

TEST(Stdlib_Concurrency_Shared, there_is_no_accessor_other_than_lock) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;
    class Counter { public var n: i32; init() { this.n = 0; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      return s.get().n;
    }
  )"),
               SunError);
}

// A guard cannot be conjured up, only handed out by lock()
TEST(Stdlib_Concurrency_Shared, a_guard_cannot_be_built_by_hand) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;
    class Counter { public var n: i32; init() { this.n = 0; } }
    function main() i32 {
      var g = SharedGuard<Counter>(null);
      return 0;
    }
  )"),
               SunError);
}
