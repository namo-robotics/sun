// tests/test_map.cpp - Tests for Map<K, V> hash map

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Map Operations
// ============================================================================

TEST(MapTest, create_empty_map) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        return m.size();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(MapTest, insert_and_get) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        return m.get(42);
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(MapTest, insert_multiple_and_get) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(1, 10);
        m.insert(2, 20);
        m.insert(3, 30);
        return m.get(1) + m.get(2) + m.get(3);
    }
  )");
  EXPECT_EQ(value, 60);
}

TEST(MapTest, update_existing_key) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        m.insert(42, 200);
        return m.get(42);
    }
  )");
  EXPECT_EQ(value, 200);
}

TEST(MapTest, contains_key) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        
        var result: i32 = 0;
        if (m.contains(42)) {
            result = result + 1;
        }
        if (m.contains(99)) {
            result = result + 10;
        }
        return result;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(MapTest, size_tracking) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        
        var s1: i64 = m.size();
        m.insert(1, 10);
        var s2: i64 = m.size();
        m.insert(2, 20);
        var s3: i64 = m.size();
        m.insert(1, 15);  // Update, not new
        var s4: i64 = m.size();
        
        return s1 + s2 * 10 + s3 * 100 + s4 * 1000;
    }
  )");
  EXPECT_EQ(value, 2210);  // 0 + 10 + 200 + 2000
}

// ============================================================================
// Get with Default
// ============================================================================

TEST(MapTest, get_or_default_found) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        return m.getOrDefault(42, -1);
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(MapTest, get_or_default_not_found) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        return m.getOrDefault(99, -1);
    }
  )");
  EXPECT_EQ(value, -1);
}

// ============================================================================
// Remove Operations
// ============================================================================

TEST(MapTest, remove_existing) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        var removed = m.remove(42);
        
        // Should have size 0 and return removed value
        var result: i32 = removed;
        if (m.size() == 0) {
            result = result + 1000;
        }
        return result;
    }
  )");
  EXPECT_EQ(value, 1100);  // 100 (removed value) + 1000 (size is 0)
}

TEST(MapTest, remove_then_contains) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        m.remove(42);
        
        if (m.contains(42)) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(MapTest, remove_and_reinsert) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        m.remove(42);
        m.insert(42, 200);
        return m.get(42);
    }
  )");
  EXPECT_EQ(value, 200);
}

// ============================================================================
// Clear Operation
// ============================================================================

TEST(MapTest, clear) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(1, 10);
        m.insert(2, 20);
        m.insert(3, 30);
        m.clear();
        
        var result: i64 = m.size();
        if (m.contains(1)) {
            result = result + 100;
        }
        return result;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Auto-resize (Grow) Tests
// ============================================================================

TEST(MapTest, auto_grow) {
  // Insert more than initial capacity (16) to trigger resize
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        
        // Insert 20 elements (should trigger resize at ~11-12)
        for (var i: i32 = 0; i < 20; i = i + 1) {
            m.insert(i, i * 10);
        }
        
        // Verify all elements are retrievable
        var sum: i32 = 0;
        for (var i: i32 = 0; i < 20; i = i + 1) {
            sum = sum + m.get(i);
        }
        
        // sum = 0 + 10 + 20 + ... + 190 = 10 * (0+1+...+19) = 10 * 190 = 1900
        return sum;
    }
  )");
  EXPECT_EQ(value, 1900);
}

TEST(MapTest, many_elements) {
  // Insert 100 elements
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        
        for (var i: i64 = 0; i < 100; i = i + 1) {
            m.insert(i, 1);
        }
        
        // Sum all values
        var sum: i32 = 0;
        for (var i: i64 = 0; i < 100; i = i + 1) {
            sum = sum + m.get(i);
        }
        
        return m.size();
    }
  )");
  EXPECT_EQ(value, 100);
}

// ============================================================================
// Custom Initial Capacity
// ============================================================================

TEST(MapTest, custom_capacity) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 64);
        
        if (m.capacity() == 64) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST(MapTest, get_missing_key_throws) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        
        try {
            var val = m.get(99);  // Key doesn't exist
            return val;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, -1);
}

TEST(MapTest, remove_missing_key_throws) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(42, 100);
        
        try {
            var val = m.remove(99);  // Key doesn't exist
            return val;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, -1);
}

// ============================================================================
// Collision Handling Tests
// ============================================================================

TEST(MapTest, hash_collision_handling) {
  // Insert keys that might collide in small table
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 4);  // Small capacity to force collisions
        
        m.insert(0, 1);
        m.insert(4, 2);   // Likely same bucket as 0 (mod 4)
        m.insert(8, 3);   // Likely same bucket as 0 and 4
        m.insert(12, 4);  // Likely same bucket
        
        return m.get(0) + m.get(4) + m.get(8) + m.get(12);
    }
  )");
  EXPECT_EQ(value, 10);  // 1 + 2 + 3 + 4
}

// ============================================================================
// Iteration Tests
// ============================================================================

TEST(MapTest, iteration_sum_values) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        m.insert(1, 10);
        m.insert(2, 20);
        m.insert(3, 30);
        
        var sum: i32 = 0;
        for (var v: i32 in m) {
            println_i32(v);
            sum = sum + v;
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 60);
}

TEST(MapTest, iteration_empty_map) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i64, i32>(allocator, 16);
        
        var count: i32 = 0;
        for (var v: i32 in m) {
            println_i32(v);
            count = count + 1;
        }
        return count;
    }
  )");
  EXPECT_EQ(value, 0);
}
