// tests/stdlib/test_option.cpp - Stage 3: Option<T>/Result<T,E> in the
// stdlib and the migrated absence-signaling APIs.

#include <gtest/gtest.h>

#include "execution_utils.h"

// ============================================================================
// Option and Result from the stdlib bundle
// ============================================================================

TEST(Stdlib_Option, OptionFromStdlib) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var a = Option.Some(40);
        var b: Option<i32> = Option.None;
        var av = match a {
            Option.Some(v) => v,
            Option.None => 0
        };
        var bv = match b {
            Option.Some(v) => v,
            Option.None => 2
        };
        return av + bv;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Option, ResultFromStdlib) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function parse_positive(x: i32) Result<i32, i32> {
        if (x < 0) { return Result.Err(-1); }
        return Result.Ok(x);
    }

    function main() i32 {
        var good = parse_positive(41);
        var bad = parse_positive(-5);
        var g = match good {
            Result.Ok(v) => v,
            Result.Err(e) => 0
        };
        var b = match bad {
            Result.Ok(v) => 0,
            Result.Err(e) => 1
        };
        return g + b;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Migrated stdlib APIs
// ============================================================================

// find() borrows the stored value: the map still owns it
TEST(Stdlib_Option, MapFind) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(1, 40);
        var hit = match m.find(1) {
            Option.Some(v) => v,
            Option.None => 0
        };
        var miss = match m.find(99) {
            Option.Some(v) => 0,
            Option.None => 2
        };
        return hit + miss;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Option, StringFindChar) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "hello world");
        var first = match s.find_char(111) {   // first 'o' at 4
            Option.Some(i) => i,
            Option.None => -1
        };
        var last = match s.rfind_char(111) {   // last 'o' at 7
            Option.Some(i) => i,
            Option.None => -1
        };
        var missing = match s.find_char(120) { // no 'x'
            Option.Some(i) => i,
            Option.None => 31
        };
        return first + last + missing;         // 4 + 7 + 31
    }
  )");
  EXPECT_EQ(value, 42);
}

// first()/last() borrow the element: the Vec still owns it
TEST(Stdlib_Option, VecFirstLast) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i32>(allocator, 8);
        var empty = match v.first() {
            Option.Some(x) => 0,
            Option.None => 2
        };
        v.push(10);
        v.push(30);
        var f = match v.first() {
            Option.Some(x) => x,
            Option.None => 0
        };
        var l = match v.last() {
            Option.Some(x) => x,
            Option.None => 0
        };
        return empty + f + l;   // 2 + 10 + 30
    }
  )");
  EXPECT_EQ(value, 42);
}

// A new user specialization of a stdlib generic whose methods return Option
TEST(Stdlib_Option, NewVecSpecializationWithOption) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<u32>(allocator, 4);
        v.push(42);
        return match v.last() {
            Option.Some(x) => x,
            Option.None => 0
        };
    }
  )");
  EXPECT_EQ(value, 42);
}

// Interface payloads: Vec<IValue>.first() yields Option<IValue>
TEST(Stdlib_Option, InterfacePayloadOption) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    interface IValue {
      function get() i32;
    }
    class Num implements IValue {
      var n: i32;
      function init(v: i32) { this.n = v; }
      function get() i32 { return this.n; }
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      var items = Vec<IValue>(alloc, 4);
      items.push(Num(42));
      return match items.first() {
        Option.Some(item) => item.get(),
        Option.None => 0
      };
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Remaining absence APIs: LinkedList.first/last, Vec.pop, iterator next()
// ============================================================================

TEST(Stdlib_Option, LinkedListFirstLast) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        var empty = match ll.first() {
            Option.Some(x) => 0,
            Option.None => 2
        };
        ll.push_back(10);
        ll.push_back(30);
        var f = match ll.first() {
            Option.Some(x) => x,
            Option.None => 0
        };
        var l = match ll.last() {
            Option.Some(x) => x,
            Option.None => 0
        };
        return empty + f + l;   // 2 + 10 + 30
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Option, VecPop) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i32>(allocator, 4);
        v.push(30);
        v.push(10);
        var a = match v.pop() { Option.Some(x) => x, Option.None => 0 };
        var b = match v.pop() { Option.Some(x) => x, Option.None => 0 };
        var c = match v.pop() { Option.Some(x) => 0, Option.None => 2 };
        return a + b + c + v.size();   // 10 + 30 + 2 + 0
    }
  )");
  EXPECT_EQ(value, 42);
}

// Iterators are driven by next() -> Option<T>; the sequence ends at None
TEST(Stdlib_Option, IteratorNextReturnsOption) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i32>(allocator, 4);
        v.push(20);
        v.push(22);
        var it = v.iter();
        var sum: i32 = 0;
        var going = true;
        while (going) {
            match it.next(v) {
                Option.Some(x) => { sum = sum + x; },
                Option.None => { going = false; }
            };
        }
        // Exhausted iterators keep returning None
        return match it.next(v) {
            Option.Some(x) => 0,
            Option.None => sum
        };
    }
  )");
  EXPECT_EQ(value, 42);
}

// for-in over every stdlib container goes through next() -> Option<T>
TEST(Stdlib_Option, ForInOverStdlibContainers) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i32>(allocator, 4);
        v.push(1);
        v.push(2);
        var ll = LinkedList<i32>(allocator);
        ll.push_back(3);
        ll.push_back(4);
        var m = Map<i64, i32>(allocator, 8);
        m.insert(1, 5);
        m.insert(2, 6);
        var buf = ContiguousBuffer<i32>(allocator, 2);
        buf[0] = 7;
        buf[1] = 14;
        var sum: i32 = 0;
        for (var x: i32 in v) { sum = sum + x; }
        for (var x: i32 in ll) { sum = sum + x; }
        for (var x: i32 in m) { sum = sum + x; }
        for (var x: i32 in buf) { sum = sum + x; }
        return sum;   // 3 + 7 + 11 + 21
    }
  )");
  EXPECT_EQ(value, 42);
}

// for-in over an empty container never enters the body
TEST(Stdlib_Option, ForInOverEmptyContainer) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        var count: i32 = 42;
        for (var x: i32 in ll) { count = 0; }
        return count;
    }
  )");
  EXPECT_EQ(value, 42);
}
