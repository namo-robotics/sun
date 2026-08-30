// tests/stdlib/collections/test_vec.cpp - Tests for Vec<T> dynamic array

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Basic Vec Operations
// ============================================================================

TEST(Stdlib_Collections_Vec, create_empty_vec) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        return v.size();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Collections_Vec, push_and_size) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(10);
        v.push(20);
        v.push(30);
        return v.size();
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Stdlib_Collections_Vec, push_and_get) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(10);
        v.push(20);
        v.push(30);
        return v.get(0) + v.get(1) + v.get(2);
    }
  )");
  EXPECT_EQ(value, 60);
}

TEST(Stdlib_Collections_Vec, set_element) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(10);
        v.push(20);
        v.set(0, 99);
        return v.get(0);
    }
  )");
  EXPECT_EQ(value, 99);
}

TEST(Stdlib_Collections_Vec, pop_element) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(10);
        v.push(20);
        v.push(30);
        var last = match v.pop() {
            Option.Some(x) => x,
            Option.None => -1
        };
        return last + v.size();
    }
  )");
  // last=30, size after pop=2, so 30+2=32
  EXPECT_EQ(value, 32);
}

TEST(Stdlib_Collections_Vec, pop_empty_is_none) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(1);
        v.pop();
        return match v.pop() {
            Option.Some(x) => 1,
            Option.None => 42
        };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Collections_Vec, is_empty) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        if (v.is_empty()) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Stdlib_Collections_Vec, clear) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(1);
        v.push(2);
        v.push(3);
        v.clear();
        return v.size();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Collections_Vec, capacity) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        return v.capacity();
    }
  )");
  // Default capacity is 8
  EXPECT_EQ(value, 8);
}

TEST(Stdlib_Collections_Vec, auto_grow) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        // Push more than initial capacity (8) to trigger grow
        for (var i: i64 = 0; i < 20; i = i + 1) {
            v.push(i);
        }
        return v.size();
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(Stdlib_Collections_Vec, auto_grow_values_preserved) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        for (var i: i64 = 0; i < 20; i = i + 1) {
            v.push(i * 10);
        }
        // Check first, middle, and last
        return v.get(0) + v.get(10) + v.get(19);
    }
  )");
  // 0 + 100 + 190 = 290
  EXPECT_EQ(value, 290);
}

TEST(Stdlib_Collections_Vec, first_and_last) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(10);
        v.push(20);
        v.push(30);
        var f = match v.first() {
            Option.Some(x) => x,
            Option.None => -1
        };
        var l = match v.last() {
            Option.Some(x) => x,
            Option.None => -1
        };
        return f + l;
    }
  )");
  // 10 + 30 = 40
  EXPECT_EQ(value, 40);
}

TEST(Stdlib_Collections_Vec, i32_type) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i32>(allocator, 8);
        v.push(5);
        v.push(10);
        return v.get(0) + v.get(1);
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(Stdlib_Collections_Vec, iteration) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(1);
        v.push(2);
        v.push(3);
        v.push(4);

        var sum: i64 = 0;
        for (var x: i64 in v) {
            sum = sum + x;
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(Stdlib_Collections_Vec, index_operator) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(100);
        v.push(200);
        return v[0] + v[1];
    }
  )");
  EXPECT_EQ(value, 300);
}
