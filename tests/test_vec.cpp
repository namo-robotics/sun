// tests/test_vec.cpp - Tests for Vec<T> dynamic array

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Vec Operations
// ============================================================================

TEST(VecTest, create_empty_vec) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        return v.size();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(VecTest, push_and_size) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, push_and_get) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, set_element) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, pop_element) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(10);
        v.push(20);
        v.push(30);
        try {
            var last = v.pop();
            return last + v.size();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // last=30, size after pop=2, so 30+2=32
  EXPECT_EQ(value, 32);
}

TEST(VecTest, is_empty) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        if (v.isEmpty()) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(VecTest, clear) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, capacity) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        return v.capacity();
    }
  )");
  // Default capacity is 8
  EXPECT_EQ(value, 8);
}

TEST(VecTest, auto_grow) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, auto_grow_values_preserved) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, first_and_last) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 8);
        v.push(10);
        v.push(20);
        v.push(30);
        try {
            return v.first() + v.last();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // 10 + 30 = 40
  EXPECT_EQ(value, 40);
}

TEST(VecTest, i32_type) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, iteration) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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

TEST(VecTest, index_operator) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

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
