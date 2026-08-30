// tests/memory_safety/test_linked_list.cpp

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// C-style Linked List Tests using allocator.create<T> for heap allocation
// ============================================================================

TEST(MemorySafety_LinkedList, two_nodes_linked) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        init(v: i32) {
            this.value = v;
            this.next = null;
        }

        method get_value() i32 {
            return this.value;
        }

        method set_next(n: raw_ptr<Node>) void {
            this.next = n;
        }

        method get_next() raw_ptr<Node> {
            return this.next;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var first = allocator.create<Node>(10);
        var second = allocator.create<Node>(20);
        unsafe { first.set_next(second); };
        return unsafe { first.get_value(); } + unsafe { first.get_next().get_value(); };
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(MemorySafety_LinkedList, three_nodes_chain) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        init(v: i32) {
            this.value = v;
            this.next = null;
        }

        method get_value() i32 {
            return this.value;
        }

        method set_next(n: raw_ptr<Node>) void {
            this.next = n;
        }

        method get_next() raw_ptr<Node> {
            return this.next;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var n1 = allocator.create<Node>(1);
        // Build chain: n1 -> n2 -> n3
        // After set_next, access through the chain, not original vars
        unsafe { n1.set_next(createNode(allocator, 2)); };
        unsafe { n1.get_next().set_next(createNode(allocator, 3)); };

        // Traverse: n1 -> n2 -> n3
        var sum = unsafe { n1.get_value(); };
        sum = sum + unsafe { n1.get_next().get_value(); };
        sum = sum + unsafe { n1.get_next().get_next().get_value(); };
        return sum;
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(MemorySafety_LinkedList, modify_through_pointer) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        init(v: i32) {
            this.value = v;
            this.next = null;
        }

        method get_value() i32 {
            return this.value;
        }

        method set_value(v: i32) void {
            this.value = v;
        }

        method set_next(n: raw_ptr<Node>) void {
            this.next = n;
        }

        method get_next() raw_ptr<Node> {
            return this.next;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var head = allocator.create<Node>(100);
        unsafe { head.set_next(createNode(allocator, 200)); };

        // Modify tail through head's next pointer
        unsafe { head.get_next().set_value(999); };

        // Access through the chain (not through moved var)
        return unsafe { head.get_next().get_value(); };
    }
  )");
  EXPECT_EQ(value, 999);
}

TEST(MemorySafety_LinkedList, access_deep_chain) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        init(v: i32) {
            this.value = v;
            this.next = null;
        }

        method get_value() i32 {
            return this.value;
        }

        method set_next(n: raw_ptr<Node>) void {
            this.next = n;
        }

        method get_next() raw_ptr<Node> {
            return this.next;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var a = allocator.create<Node>(1);
        // Build chain: a -> b -> c -> d
        unsafe { a.set_next(createNode(allocator, 2)); };
        unsafe { a.get_next().set_next(createNode(allocator, 3)); };
        unsafe { a.get_next().get_next().set_next(createNode(allocator, 4)); };

        // Access 4th element: a -> b -> c -> d
        return unsafe { a.get_next().get_next().get_next().get_value(); };
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(MemorySafety_LinkedList, null_terminated_list) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        init(v: i32) {
            this.value = v;
            this.next = null;
        }

        method get_value() i32 {
            return this.value;
        }

        method set_next(n: raw_ptr<Node>) void {
            this.next = n;
        }

        method get_next() raw_ptr<Node> {
            return this.next;
        }

        method hasNext() bool {
            return this.next != null;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var n1 = allocator.create<Node>(10);
        // Build chain without reusing moved vars
        unsafe { n1.set_next(createNode(allocator, 20)); };
        unsafe { n1.get_next().set_next(createNode(allocator, 30)); };
        // n3.next is null by default from init

        // Sum all values by checking for null
        var sum = unsafe { n1.get_value(); };
        if (unsafe { n1.hasNext(); }) {
            sum = sum + unsafe { n1.get_next().get_value(); };
            if (unsafe { n1.get_next().hasNext(); }) {
                sum = sum + unsafe { n1.get_next().get_next().get_value(); };
            }
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 60);
}

TEST(MemorySafety_LinkedList, while_loop_traversal) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        init(v: i32) {
            this.value = v;
            this.next = null;
        }

        method get_value() i32 {
            return this.value;
        }

        method set_next(n: raw_ptr<Node>) void {
            this.next = n;
        }

        method get_next() raw_ptr<Node> {
            return this.next;
        }

        method hasNext() bool {
            return this.next != null;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        // Build list: 1 -> 2 -> 3 -> 4 -> 5 -> null
        var head = allocator.create<Node>(1);
        unsafe { head.set_next(createNode(allocator, 2)); };
        unsafe { head.get_next().set_next(createNode(allocator, 3)); };
        unsafe { head.get_next().get_next().set_next(createNode(allocator, 4)); };
        unsafe { head.get_next().get_next().get_next().set_next(createNode(allocator, 5)); };

        // Traverse with while loop
        var sum = 0;
        var curr: raw_ptr<Node> = head;
        while (curr != null) {
            sum = sum + unsafe { curr.get_value(); };
            curr = unsafe { curr.get_next(); };
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(MemorySafety_LinkedList, list_class_with_methods) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        init(v: i32) {
            this.value = v;
            this.next = null;
        }

        method get_value() i32 {
            return this.value;
        }

        method set_next(n: raw_ptr<Node>) void {
            this.next = n;
        }

        method get_next() raw_ptr<Node> {
            return this.next;
        }

        method hasNext() bool {
            return this.next != null;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    class List {
        var head: raw_ptr<Node>;
        var tail: raw_ptr<Node>;

        init() {
            this.head = null;
            this.tail = null;
        }

        method append(alloc: ref HeapAllocator, v: i32) void {
            if (this.tail != null) {
                // Append to existing list - create node and link via tail
                unsafe { this.tail.set_next(createNode(alloc, v)); };
                this.tail = unsafe { this.tail.get_next(); };
            } else {
                // First node - set both head and tail
                this.head = createNode(alloc, v);
                this.tail = this.head;
            }
        }

        method sum() i32 {
            var total = 0;
            var curr: raw_ptr<Node> = this.head;
            while (curr != null) {
                total = total + unsafe { curr.get_value(); };
                curr = unsafe { curr.get_next(); };
            }
            return total;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var list = allocator.create<List>();
        unsafe { list.append(allocator, 1); };
        unsafe { list.append(allocator, 2); };
        unsafe { list.append(allocator, 3); };
        unsafe { list.append(allocator, 4); };
        unsafe { list.append(allocator, 5); };
        return unsafe { list.sum(); };
    }
  )");
  EXPECT_EQ(value, 15);
}