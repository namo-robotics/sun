// tests/test_stdlib_linked_list.cpp - Tests for stdlib LinkedList<T>

#include <gtest/gtest.h>

#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Operations
// ============================================================================

TEST(StdlibLinkedListTest, empty_list) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        if (ll.isEmpty()) {
            return ll.size();
        }
        return -1;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// push_back / push_front
// ============================================================================

TEST(StdlibLinkedListTest, push_back) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(10);
        ll.push_back(20);
        ll.push_back(30);
        try {
            var f = ll.first();
            var l = ll.last();
            return f + l + ll.size();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // first=10, last=30, size=3 -> 43
  EXPECT_EQ(value, 43);
}

TEST(StdlibLinkedListTest, push_front) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_front(3);
        ll.push_front(2);
        ll.push_front(1);
        // List: 1 -> 2 -> 3
        try {
            var f = ll.first();
            var l = ll.last();
            return f * 100 + l;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // first=1, last=3 -> 103
  EXPECT_EQ(value, 103);
}

// ============================================================================
// Pop Operations
// ============================================================================

TEST(StdlibLinkedListTest, pop_front) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(10);
        ll.push_back(20);
        ll.push_back(30);
        try {
            var v = ll.pop_front();
            return v * 10 + ll.size();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // popped=10, size=2 -> 102
  EXPECT_EQ(value, 102);
}

TEST(StdlibLinkedListTest, pop_back) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(10);
        ll.push_back(20);
        ll.push_back(30);
        try {
            var v = ll.pop_back();
            return v * 10 + ll.size();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // popped=30, size=2 -> 302
  EXPECT_EQ(value, 302);
}

TEST(StdlibLinkedListTest, pop_empty_throws) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        var result = 0;
        try {
            var v = ll.pop_front();
        } catch (e: IError) {
            result = result + 1;
        }
        try {
            var v = ll.pop_back();
        } catch (e: IError) {
            result = result + 1;
        }
        return result;
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(StdlibLinkedListTest, drain_list) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(1);
        ll.push_back(2);
        ll.push_back(3);
        var sum = 0;
        try {
            sum = sum + ll.pop_front();
            sum = sum + ll.pop_back();
            sum = sum + ll.pop_front();
        } catch (e: IError) {
            return -1;
        }
        if (ll.isEmpty()) {
            return sum;
        }
        return -2;
    }
  )");
  // 1 + 3 + 2 = 6
  EXPECT_EQ(value, 6);
}

// ============================================================================
// Indexed Access
// ============================================================================

TEST(StdlibLinkedListTest, get_and_set) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(10);
        ll.push_back(20);
        ll.push_back(30);
        ll.push_back(40);
        try {
            var first = ll.get(0);
            var last = ll.get(3);
            ll.set(1, 99);
            var modified = ll.get(1);
            return first + last + modified;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // 10 + 40 + 99 = 149
  EXPECT_EQ(value, 149);
}

TEST(StdlibLinkedListTest, get_out_of_bounds) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(10);
        try {
            var v = ll.get(5);
            return 0;
        } catch (e: IError) {
            return 1;
        }
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Remove
// ============================================================================

TEST(StdlibLinkedListTest, remove_nodes) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(10);
        ll.push_back(20);
        ll.push_back(30);
        ll.push_back(40);
        ll.push_back(50);
        try {
            var mid = ll.remove(2);
            var first = ll.remove(0);
            var last = ll.remove(2);
            // Remaining: 20 -> 40
            return mid + first + last + ll.size();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // 30 + 10 + 50 + 2 = 92
  EXPECT_EQ(value, 92);
}

// ============================================================================
// Clear
// ============================================================================

TEST(StdlibLinkedListTest, clear_and_reuse) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(10);
        ll.push_back(20);
        ll.push_back(30);
        ll.clear();
        ll.push_back(99);
        try {
            var f = ll.first();
            return f + ll.size();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // first=99, size=1 -> 100
  EXPECT_EQ(value, 100);
}

// ============================================================================
// Iteration
// ============================================================================

TEST(StdlibLinkedListTest, for_in_iteration) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(1);
        ll.push_back(2);
        ll.push_back(4);
        ll.push_back(8);

        // Positional weighted sum to verify order
        var result = 0;
        var multiplier = 1;
        for (var v: i32 in ll) {
            result = result + v * multiplier;
            multiplier = multiplier * 10;
        }
        // 1*1 + 2*10 + 4*100 + 8*1000 = 8421
        return result;
    }
  )");
  EXPECT_EQ(value, 8421);
}

TEST(StdlibLinkedListTest, iterate_empty) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        var sum = 0;
        for (var v: i32 in ll) {
            sum = sum + v;
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Mixed Operations
// ============================================================================

TEST(StdlibLinkedListTest, mixed_push_front_back) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(2);
        ll.push_front(1);
        ll.push_back(3);
        ll.push_front(0);
        // List: 0 -> 1 -> 2 -> 3

        var sum = 0;
        for (var v: i32 in ll) {
            sum = sum + v;
        }
        return sum + ll.size();
    }
  )");
  // 0+1+2+3 + 4 = 10
  EXPECT_EQ(value, 10);
}

TEST(StdlibLinkedListTest, i64_type) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i64>(allocator);
        ll.push_back(100);
        ll.push_back(200);
        ll.push_back(300);
        try {
            var sum: i64 = ll.pop_front() + ll.pop_back();
            return sum;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // 100 + 300 = 400
  EXPECT_EQ(value, 400);
}

// ============================================================================
// List of Class Objects
// ============================================================================

TEST(StdlibLinkedListTest, list_of_class_objects) {
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

        function getX() i32 {
            return this.x;
        }

        function getY() i32 {
            return this.y;
        }

        function sum() i32 {
            return this.x + this.y;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var list = LinkedList<Point>(allocator);

        list.push_back(Point(1, 2));
        list.push_back(Point(3, 4));
        list.push_back(Point(5, 6));

        // Access via get
        try {
            var p0 = list.get(0);
            var p1 = list.get(1);
            var p2 = list.get(2);
            return p0.sum() + p1.sum() + p2.sum();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // (1+2) + (3+4) + (5+6) = 3 + 7 + 11 = 21
  EXPECT_EQ(value, 21);
}

TEST(StdlibLinkedListTest, class_objects_push_front_pop) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Pair {
        var a: i32;
        var b: i32;

        function init(a_: i32, b_: i32) {
            this.a = a_;
            this.b = b_;
        }

        function product() i32 {
            return this.a * this.b;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var list = LinkedList<Pair>(allocator);

        list.push_back(Pair(2, 3));
        list.push_back(Pair(4, 5));
        list.push_front(Pair(1, 10));
        // List: (1,10) -> (2,3) -> (4,5)

        try {
            var front = list.pop_front();
            var back = list.pop_back();
            return front.product() + back.product();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  // front=(1,10)->10, back=(4,5)->20 => 30
  EXPECT_EQ(value, 30);
}

TEST(StdlibLinkedListTest, iterate_class_objects) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Weight {
        var value: i32;

        function init(v: i32) {
            this.value = v;
        }

        function getValue() i32 {
            return this.value;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var list = LinkedList<Weight>(allocator);

        list.push_back(Weight(10));
        list.push_back(Weight(20));
        list.push_back(Weight(30));
        list.push_back(Weight(40));

        var sum = 0;
        for (var w: Weight in list) {
            sum = sum + w.getValue();
        }
        return sum;
    }
  )");
  // 10+20+30+40 = 100
  EXPECT_EQ(value, 100);
}

// ============================================================================
// Memory Safety - known UB scenarios (borrow checker limitations)
// ============================================================================

// Use-after-free: stale raw_ptr to a node survives pop_front freeing it.
// The borrow checker does NOT track raw_ptr lifetimes, so this compiles
// and executes without error — but dereferences freed memory (UB).
TEST(StdlibLinkedListTest, DISABLED_use_after_free_stale_raw_ptr) {
  // This test demonstrates UB that the borrow checker cannot catch.
  // The stale raw_ptr reads freed memory. In practice this may return
  // the old value (memory not yet reclaimed) or garbage.
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(42);

        // Alias the internal head pointer
        var stale: raw_ptr<LinkedListNode<i32>> = ll.head;

        // Free the node via pop
        try { var v = ll.pop_front(); } catch (e: IError) { return -1; }

        // UB: stale points to freed memory
        return stale.getValue();
    }
  )");
  // UB: may return 42 (stale read) or anything else
  (void)value;
}

// Double-free: passing a LinkedList by value into a struct copies the
// raw_ptr fields (head/tail). Both the original and the copy call deinit,
// freeing the same nodes twice. The borrow checker does NOT prevent this.
TEST(StdlibLinkedListTest, DISABLED_double_free_via_copy) {
  // This test crashes with "free(): invalid pointer" — confirmed double-free.
  // Disabled because it aborts the test process.
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class ListWrapper {
        var inner: LinkedList<i32>;

        function init(list: LinkedList<i32>) {
            this.inner = list;
        }

        function getSize() i64 {
            return this.inner.size();
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(1);
        ll.push_back(2);

        var wrapper = ListWrapper(ll);
        // ll is bitwise-copied into wrapper; both deinit -> double free
        return ll.size();
    }
  )");
  (void)value;
}
