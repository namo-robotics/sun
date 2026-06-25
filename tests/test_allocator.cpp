// tests/test_allocator.cpp - Tests for allocator-based memory allocation

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Minimal Variadic Parameter Tests (no stdlib)
// ============================================================================

TEST(AllocatorTest, variadic_method_local_allocator) {
  // Minimal test: local class with variadic generic method
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) { this.x = x; this.y = y; }
        function sum() i32 { return this.x + this.y; }
    }

    class MyAllocator {
        function init() {}
        function create<T>(args...: _init_args<T>) raw_ptr<T> {
            var size: i64 = _sizeof<T>();
            var memory: raw_ptr<i8> = unsafe { _malloc(size); };
            _init<T>(memory, args...);
            return memory;
        }
    }

    function main() i32 {
        var alloc = MyAllocator();
        var p = alloc.create<Point>(3, 4);
        return unsafe { p.sum(); };
    }
  )");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Basic HeapAllocator Tests
// ============================================================================

TEST(AllocatorTest, heap_allocator_create_simple_class) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    class Point {
        var x: i32;
        var y: i32;
        
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        }
        
        function sum() i32 {
            return this.x + this.y;
        }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
      var rawPtr = alloc.create<Point>(3, 4);
        var p = Unique<Point>(rawPtr);
        return p.get().sum();
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(AllocatorTest, heap_allocator_create_generic_class) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    class Box<T> {
        var value: T;
        
        function init(v: T) {
            this.value = v;
        }
        
        function get() T {
            return this.value;
        }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var rawPtr = alloc.create<Box<i32>>(42);
        var p = Unique<Box<i32>>(rawPtr);
        return p.get().get();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(AllocatorTest, unique_ptr_automatic_cleanup) {
  // Unique<T> wraps allocator.create<T>() result for automatic cleanup
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    class Point {
        var x: i32;
        var y: i32;
        
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var rawPtr = alloc.create<Point>(3, 4);
        var p = Unique<Point>(rawPtr);
        var result = p.get().x + p.get().y;
        // p.deinit() is automatically called at scope exit
        return result;
    }
  )");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Multiple Heap Allocations
// ============================================================================

TEST(AllocatorTest, multiple_heap_allocations) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    class Counter {
        var value: i32;
        
        function init(v: i32) {
            this.value = v;
        }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var c1 = Unique<Counter>(alloc.create<Counter>(10));
        var c2 = Unique<Counter>(alloc.create<Counter>(20));
        var c3 = Unique<Counter>(alloc.create<Counter>(30));
        return c1.get().value + c2.get().value + c3.get().value;
    }
  )");
  EXPECT_EQ(value, 60);
}

// ============================================================================
// Allocator Passed as Parameter
// ============================================================================

TEST(AllocatorTest, allocator_as_parameter) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    class Point {
        var x: i32;
        var y: i32;
        
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        }
    }
    
    function create_point(alloc: ref HeapAllocator, x: i32, y: i32) raw_ptr<Point> {
        return alloc.create<Point>(x, y);
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var p = Unique<Point>(create_point(alloc, 5, 7));
        return p.get().x + p.get().y;
    }
  )");
  EXPECT_EQ(value, 12);
}

// ============================================================================
// Intrinsic Tests
// ============================================================================

TEST(AllocatorTest, sizeof_intrinsic_i32) {
  auto value = executeString(R"(
    function main() i64 {
        return _sizeof<i32>();
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(AllocatorTest, sizeof_intrinsic_i64) {
  auto value = executeString(R"(
    function main() i64 {
        return _sizeof<i64>();
    }
  )");
  EXPECT_EQ(value, 8);
}

TEST(AllocatorTest, sizeof_intrinsic_class) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init() {}
    }
    
    function main() i64 {
        return _sizeof<Point>();
    }
  )");
  EXPECT_EQ(value, 8);  // Two i32 fields = 8 bytes
}

TEST(AllocatorTest, init_intrinsic_with_allocator) {
  // Test _init<T> with allocator-allocated memory
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    class Point {
        var x: i32;
        var y: i32;
        
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        }
        
        function sum() i32 {
            return this.x + this.y;
        }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var p = alloc.create<Point>(10, 20);
        return unsafe { p.sum(); };
    }
  )");
  EXPECT_EQ(value, 30);  // 10 + 20
}

// ============================================================================
// Overloaded constructors via _init_args<T> variadic factory
// ============================================================================

TEST(AllocatorTest, create_selects_init_overload_by_args) {
  // The same create<Point> specialization site is used with two different
  // arities; each must select the matching init overload.
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) { this.x = x; this.y = y; }
        function init(v: i32) { this.x = v; this.y = v; }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var a = Unique<Point>(alloc.create<Point>(7));     // init(i32) -> 14
        var b = Unique<Point>(alloc.create<Point>(3, 4));  // init(i32,i32) -> 7
        return a.get().x + a.get().y + b.get().x + b.get().y;
    }
  )");
  EXPECT_EQ(value, 21);  // 14 + 7
}

TEST(AllocatorTest, create_single_arg_init_overload) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) { this.x = x; this.y = y; }
        function init(v: i32) { this.x = v; this.y = v; }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var p = Unique<Point>(alloc.create<Point>(9));
        return p.get().x + p.get().y;  // 9 + 9
    }
  )");
  EXPECT_EQ(value, 18);
}

TEST(AllocatorTest, create_no_matching_init_overload_errors) {
  // No 3-arg init exists; this must be a clean semantic error, not a crash.
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;

    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) { this.x = x; this.y = y; }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        var p = Unique<Point>(alloc.create<Point>(1, 2, 3));
        return 0;
    }
  )"),
               SunError);
}
