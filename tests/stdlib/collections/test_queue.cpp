// tests/stdlib/collections/test_queue.cpp - Tests for stdlib Queue<T>

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Basic Operations
// ============================================================================

TEST(Stdlib_Collections_Queue, empty_queue) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        if (q.is_empty()) {
            return _convert<i32>(q.size());
        }
        return -1;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Collections_Queue, push_grows_the_queue) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        q.push(10);
        q.push(20);
        q.push(30);
        return _convert<i32>(q.size());
    }
  )");
  EXPECT_EQ(value, 3);
}

// The whole point of the type: what goes in first comes out first.
TEST(Stdlib_Collections_Queue, pops_in_the_order_pushed) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        q.push(1);
        q.push(2);
        q.push(3);

        var order: i32 = 0;
        while (q.is_empty() == false) {
            match (q.pop()) {
                Option.Some(v) => { order = order * 10 + v; },
                Option.None => {},
            };
        }
        return order;
    }
  )");
  EXPECT_EQ(value, 123);
}

TEST(Stdlib_Collections_Queue, pop_on_empty_is_none) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        var result: i32 = -2;
        match (q.pop()) {
            Option.Some(v) => { result = -1; },
            Option.None => { result = 42; },
        };
        return result;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Collections_Queue, push_and_pop_interleaved) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        q.push(1);
        q.push(2);

        var first: i32 = 0;
        match (q.pop()) {
            Option.Some(v) => { first = v; },
            Option.None => {},
        };

        q.push(3);

        // 2 was queued before 3, so it leaves first.
        var second: i32 = 0;
        match (q.pop()) {
            Option.Some(v) => { second = v; },
            Option.None => {},
        };
        return first * 100 + second * 10 + _convert<i32>(q.size());
    }
  )");
  EXPECT_EQ(value, 121);
}

// ============================================================================
// peek
// ============================================================================

TEST(Stdlib_Collections_Queue, peek_borrows_the_front_without_removing_it) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        q.push(7);
        q.push(8);

        var front: i32 = 0;
        match (q.peek()) {
            Option.Some(v) => { front = v; },
            Option.None => {},
        };
        // Still two items: peek borrows, it does not take.
        return front * 10 + _convert<i32>(q.size());
    }
  )");
  EXPECT_EQ(value, 72);
}

TEST(Stdlib_Collections_Queue, peek_back_is_the_most_recent_push) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        q.push(1);
        q.push(9);
        var result: i32 = -2;
        match (q.peek_back()) {
            Option.Some(v) => { result = v; },
            Option.None => { result = -1; },
        };
        return result;
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(Stdlib_Collections_Queue, peek_on_empty_is_none) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        var result: i32 = -2;
        match (q.peek()) {
            Option.Some(v) => { result = -1; },
            Option.None => { result = 42; },
        };
        return result;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// clear / iteration
// ============================================================================

TEST(Stdlib_Collections_Queue, clear_empties_the_queue) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        q.push(1);
        q.push(2);
        q.clear();
        if (q.is_empty()) {
            return _convert<i32>(q.size());
        }
        return -1;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Collections_Queue, iterates_front_to_back) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<i32>(allocator);
        q.push(1);
        q.push(2);
        q.push(3);

        var seen: i32 = 0;
        for (var x: i32 in q) {
            seen = seen * 10 + x;
        }
        // Iteration borrows: everything is still queued afterwards.
        return seen * 10 + _convert<i32>(q.size());
    }
  )");
  EXPECT_EQ(value, 1233);
}

// ============================================================================
// Ownership
// ============================================================================

// The queue owns what it holds: items still queued are dropped with it.
TEST(Stdlib_Collections_Queue, drops_remaining_items) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    var drops: i32 = 0;

    class Tracked {
        public var id: i32;
        init(id: i32) { this.id = id; }
        deinit() {
            // A moved-from husk is zeroed, and has nothing to release.
            if (this.id != 0) { drops = drops + 1; }
        }
    }

    function fill() void {
        var allocator = make_heap_allocator();
        var q = Queue<Tracked>(allocator);
        q.push(Tracked(1));
        q.push(Tracked(2));
        q.push(Tracked(3));
    }

    function main() i32 {
        fill();
        return drops;
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Stdlib_Collections_Queue, clear_drops_the_items_it_removes) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    var drops: i32 = 0;

    class Tracked {
        public var id: i32;
        init(id: i32) { this.id = id; }
        deinit() {
            if (this.id != 0) { drops = drops + 1; }
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var q = Queue<Tracked>(allocator);
        q.push(Tracked(1));
        q.push(Tracked(2));
        q.clear();
        // Both were dropped by clear(), before the queue itself goes away.
        return drops;
    }
  )");
  EXPECT_EQ(value, 2);
}
