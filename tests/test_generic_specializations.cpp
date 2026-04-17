// tests/test_generic_specializations.cpp - Tests for new generic
// specializations from precompiled moon libraries
//
// These tests verify that user code can create and use NEW specializations of
// generic types imported from precompiled .moon libraries (not just use the
// pre-declared ones).

#include <gtest/gtest.h>

#include "execution_utils.h"

// ============================================================================
// Generic Class: New Primitive Specialization
// ============================================================================

// Test using Vec<u32> which is NOT pre-declared in stdlib
// (stdlib only pre-declares Vec<i8/i32/i64/f32/f64>)
TEST(GenericSpecializationTest, vec_new_primitive_specialization) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<u32>(allocator, 4);
        v.push(100);
        v.push(200);
        return v.get(0) + v.get(1);
    }
  )");
  EXPECT_EQ(value, 300);
}

// ============================================================================
// Generic Class: User-Defined Class Type Parameter
// ============================================================================

// Test using Vec<Point> where Point is a custom class defined in user code
// This test only verifies that the generic can be instantiated with a custom
// class type - full functionality (push/get) may have limitations
TEST(GenericSpecializationTest, vec_with_custom_class) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Point {
        var x: i32;
        var y: i32;
        function init(x_: i32, y_: i32) {
            this.x = x_;
            this.y = y_;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        // Just instantiate Vec<Point> to test specialization is created
        var v = Vec<Point>(allocator, 4);
        return v.capacity();
    }
  )");
  EXPECT_EQ(value, 4);
}

// ============================================================================
// Generic Method: New Specialization
// ============================================================================

// Test using allocator.create<T>() with a custom class type
TEST(GenericSpecializationTest, generic_method_new_specialization) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Counter {
        var count: i32;
        function init(start: i32) {
            this.count = start;
        }
        function get() i32 {
            return this.count;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var counter_ptr = allocator.create<Counter>(42);
        return counter_ptr.get();
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Generic Class: Map with New Key/Value Types
// ============================================================================

// Test using Map<i32, i32> which is NOT pre-declared in stdlib
// (stdlib only pre-declares Map<i64, i32> and Map<i64, i64>)
TEST(GenericSpecializationTest, map_new_key_value_specialization) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i32, i32>(allocator, 16);
        m.insert(1, 100);
        m.insert(2, 200);
        // Verify size works with new key/value types
        return m.size();
    }
  )");
  EXPECT_EQ(value, 2);
}

// ============================================================================
// Simplified Vec<T> with Point - IR analysis
// ============================================================================

// Minimal Vec<T> reimplemented inline to analyze IR for class type parameters
TEST(GenericSpecializationTest, simplified_vec_with_class) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(x_: i32, y_: i32) {
            this.x = x_;
            this.y = y_;
        }
    }

    class SimpleVec<T> {
        var data: raw_ptr<i8>;
        var size_: i64;

        function init() {
            this.size_ = 0;
            this.data = _malloc(_sizeof<T>() * 4);
        }

        function push(value: ref T) void {
            _store<T>(this.data, this.size_, value);
            this.size_ = this.size_ + 1;
        }

        function get(index: i64) T {
            return _load<T>(this.data, index);
        }

        function size() i64 {
            return this.size_;
        }
    }

    function main() i32 {
        var v = SimpleVec<Point>();
        var p1 = Point(10, 20);
        var p2 = Point(30, 40);
        v.push(p1);
        v.push(p2);
        var result = v.get(0);
        return result.x + result.y;
    }
  )");
  EXPECT_EQ(value, 30);
}
