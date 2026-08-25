// tests/stdlib/concurrency/test_shared.cpp - Tests for Shared<T>, the
// atomically reference-counted shared-ownership pointer

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"
#include "support/error.h"

// A moved-from value is left all-zero and still has its deinit run, so every
// drop counter here ignores the zero state — the same discipline Unique<T>
// follows by null-checking its pointer.

// ============================================================================
// Reference counting
// ============================================================================

TEST(Stdlib_Concurrency_Shared, one_handle_starts_at_one) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Res { public var v: i32; public function init(v: i32) { this.v = v; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Res>(alloc, Res(5));
      return s.count();
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Stdlib_Concurrency_Shared, clone_raises_and_scope_exit_lowers_the_count) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Res { public var v: i32; public function init(v: i32) { this.v = v; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Res>(alloc, Res(5));
      var inner: i32 = 0;
      if (true) {
        var c = s.clone();
        inner = s.count();
      }
      return inner * 10 + s.count();
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(Stdlib_Concurrency_Shared, value_drops_only_when_the_last_handle_goes) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    var drops: i32 = 0;
    class Res {
      public var v: i32;
      public function init(v: i32) { this.v = v; }
      function deinit() void { if (this.v != 0) { drops = drops + 1; } }
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var afterClone: i32 = 0;
      if (true) {
        var s = Shared<Res>(alloc, Res(5));
        if (true) {
          var c = s.clone();
        }
        afterClone = drops;    // 0 - s still owns it
      }
      return afterClone * 10 + drops;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Stdlib_Concurrency_Shared, a_clone_keeps_the_value_alive) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Res { public var v: i32; public function init(v: i32) { this.v = v; } }
    function hand_out(alloc: const ref HeapAllocator) Shared<Res> {
      var s = Shared<Res>(alloc, Res(9));
      return s.clone();     // s drops here; the clone carries on
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var kept = hand_out(alloc);
      var g = kept.lock();
      return g.get().v * 10 + kept.count();
    }
  )");
  EXPECT_EQ(value, 91);
}

TEST(Stdlib_Concurrency_Shared, binding_a_handle_to_a_new_name_moves_it) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Res { public var v: i32; public function init(v: i32) { this.v = v; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Res>(alloc, Res(3));
      var s2 = s;            // a move, not another owner
      return s2.count();
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Stdlib_Concurrency_Shared, moved_handle_cannot_be_used) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;
    class Res { public var v: i32; public function init(v: i32) { this.v = v; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Res>(alloc, Res(3));
      var s2 = s;
      return s.count();
    }
  )"),
               SunError);
}

// A value that owns heap memory must be released exactly once, by whichever
// handle happens to be last
TEST(Stdlib_Concurrency_Shared, an_owning_value_is_released_once) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    function main() i32 {
      var alloc = make_heap_allocator();
      var total: i64 = 0;
      if (true) {
        var s = Shared<Vec<i64>>(alloc, Vec<i64>(alloc, 4));
        if (true) {
          var c = s.clone();
          var g = c.lock();
          g.get().push(11);
          g.get().push(31);
        }
        var g = s.lock();
        total = g.get().size();
      }
      return _convert<i32>(total);
    }
  )");
  EXPECT_EQ(value, 2);
}

// ============================================================================
// Locking
// ============================================================================

TEST(Stdlib_Concurrency_Shared, write_through_the_guard_and_read_it_back) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i32; public function init() { this.n = 0; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      if (true) {
        var g = s.lock();
        g.get().n = 42;
      }
      var g2 = s.lock();
      return g2.get().n;
    }
  )");
  EXPECT_EQ(value, 42);
}

// Dropping the guard unlocks, so a second lock in the same function proceeds
TEST(Stdlib_Concurrency_Shared, the_guard_unlocks_at_scope_exit) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i32; public function init() { this.n = 0; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      if (true) { var g = s.lock(); g.get().n = g.get().n + 1; }
      if (true) { var g = s.lock(); g.get().n = g.get().n + 1; }
      if (true) { var g = s.lock(); g.get().n = g.get().n + 1; }
      var g = s.lock();
      return g.get().n;
    }
  )");
  EXPECT_EQ(value, 3);
}

// Unwinding past a guard releases the lock on the way out
TEST(Stdlib_Concurrency_Shared, the_guard_unlocks_while_unwinding) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Boom implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() String { return String("boom"); }
    }
    class Counter { public var n: i32; public function init() { this.n = 0; } }
    function fail(s: const ref Shared<Counter>) i32, IError {
      var g = s.lock();
      g.get().n = 7;
      throw Boom();
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      try {
        var r = fail(s);
      } catch (e: IError) {
        var g = s.lock();      // deadlocks if the guard did not unlock
        return g.get().n;
      };
      return 0;
    }
  )");
  EXPECT_EQ(value, 7);
}

// lock() is a const function, so a read-only handle can still reach the value
TEST(Stdlib_Concurrency_Shared, lock_works_through_a_const_ref_handle) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i32; public function init() { this.n = 0; } }
    function bump(s: const ref Shared<Counter>) void {
      var g = s.lock();
      g.get().n = g.get().n + 5;
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      bump(s);
      bump(s);
      var g = s.lock();
      return g.get().n;
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// Threads
// ============================================================================

// The gap this type closes: two threads that both WRITE one value. Each owns
// its own clone, moved in through the capture list.
TEST(Stdlib_Concurrency_Shared, two_threads_write_one_value) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i64; public function init() { this.n = 0; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      var c1 = s.clone();
      var c2 = s.clone();
      var t1 = spawn(lambda [c1]() i32 {
        var i: i64 = 0;
        while (i < 20000) { var g = c1.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      var t2 = spawn(lambda [c2]() i32 {
        var i: i64 = 0;
        while (i < 20000) { var g = c2.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      t1.join();
      t2.join();
      var g = s.lock();
      return _convert<i32>(g.get().n);
    }
  )");
  EXPECT_EQ(value, 40000);
}

TEST(Stdlib_Concurrency_Shared, four_threads_write_one_value) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i64; public function init() { this.n = 0; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      var c1 = s.clone();
      var c2 = s.clone();
      var c3 = s.clone();
      var c4 = s.clone();
      var t1 = spawn(lambda [c1]() i32 {
        var i: i64 = 0;
        while (i < 5000) { var g = c1.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      var t2 = spawn(lambda [c2]() i32 {
        var i: i64 = 0;
        while (i < 5000) { var g = c2.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      var t3 = spawn(lambda [c3]() i32 {
        var i: i64 = 0;
        while (i < 5000) { var g = c3.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      var t4 = spawn(lambda [c4]() i32 {
        var i: i64 = 0;
        while (i < 5000) { var g = c4.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      t1.join();
      t2.join();
      t3.join();
      t4.join();
      var g = s.lock();
      return _convert<i32>(g.get().n);
    }
  )");
  EXPECT_EQ(value, 20000);
}

// Several threads may also share one handle read-only and lock through it
TEST(Stdlib_Concurrency_Shared, threads_share_one_handle_by_const_ref) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i64; public function init() { this.n = 0; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      var t1 = spawn(lambda [const ref s]() i32 {
        var i: i64 = 0;
        while (i < 20000) { var g = s.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      var t2 = spawn(lambda [const ref s]() i32 {
        var i: i64 = 0;
        while (i < 20000) { var g = s.lock(); g.get().n = g.get().n + 1; i = i + 1; }
        return 0;
      });
      t1.join();
      t2.join();
      var g = s.lock();
      return _convert<i32>(g.get().n);
    }
  )");
  EXPECT_EQ(value, 40000);
}

// A thread that is never joined by hand is joined when its handle's scope
// ends, and only then is the clone it captured dropped. If those ran the
// other way round the thread would be reading freed memory.
TEST(Stdlib_Concurrency_Shared,
     an_unjoined_thread_finishes_before_its_clone_drops) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i64; public function init() { this.n = 0; } }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = Shared<Counter>(alloc, Counter());
      if (true) {
        var c = s.clone();
        var t = spawn(lambda [c]() i32 {
          var i: i64 = 0;
          while (i < 50000) { var g = c.lock(); g.get().n = g.get().n + 1; i = i + 1; }
          return 0;
        });
      }
      var g = s.lock();
      return _convert<i32>(g.get().n) + s.count();
    }
  )");
  EXPECT_EQ(value, 50001);
}

// ============================================================================
// The value is reachable only through the lock
// ============================================================================

TEST(Stdlib_Concurrency_Shared, there_is_no_accessor_other_than_lock) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;
    class Counter { public var n: i32; public function init() { this.n = 0; } }
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
    using sun;
    class Counter { public var n: i32; public function init() { this.n = 0; } }
    function main() i32 {
      var g = SharedGuard<Counter>(null);
      return 0;
    }
  )"),
               SunError);
}
