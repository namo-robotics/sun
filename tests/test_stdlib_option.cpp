// tests/test_stdlib_option.cpp - Stage 3: Option<T>/Result<T,E> in the
// stdlib and the migrated absence-signaling APIs.

#include <gtest/gtest.h>

#include "execution_utils.h"

// ============================================================================
// Option and Result from the stdlib bundle
// ============================================================================

TEST(StdlibOptionTest, OptionFromStdlib) {
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

TEST(StdlibOptionTest, ResultFromStdlib) {
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

TEST(StdlibOptionTest, MapFind) {
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

TEST(StdlibOptionTest, StringFindChar) {
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

TEST(StdlibOptionTest, VecFirstLast) {
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
TEST(StdlibOptionTest, NewVecSpecializationWithOption) {
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
TEST(StdlibOptionTest, InterfacePayloadOption) {
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
