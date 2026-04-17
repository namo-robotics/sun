// tests/test_linked_list.cpp

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// C-style Linked List Tests using allocator.create<T> for heap allocation
// ============================================================================

TEST(LinkedListTest, two_nodes_linked) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        function init(v: i32) {
            this.value = v;
        }

        function getValue() i32 {
            return this.value;
        }

        function setNext(n: raw_ptr<Node>) {
            this.next = n;
        }

        function getNext() raw_ptr<Node> {
            return this.next;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var first = allocator.create<Node>(10);
        var second = allocator.create<Node>(20);
        first.setNext(second);
        return first.getValue() + first.getNext().getValue();
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(LinkedListTest, three_nodes_chain) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        function init(v: i32) {
            this.value = v;
        }

        function getValue() i32 {
            return this.value;
        }

        function setNext(n: raw_ptr<Node>) {
            this.next = n;
        }

        function getNext() raw_ptr<Node> {
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
        // After setNext, access through the chain, not original vars
        n1.setNext(createNode(allocator, 2));
        n1.getNext().setNext(createNode(allocator, 3));

        // Traverse: n1 -> n2 -> n3
        var sum = n1.getValue();
        sum = sum + n1.getNext().getValue();
        sum = sum + n1.getNext().getNext().getValue();
        return sum;
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(LinkedListTest, modify_through_pointer) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        function init(v: i32) {
            this.value = v;
        }

        function getValue() i32 {
            return this.value;
        }

        function setValue(v: i32) {
            this.value = v;
        }

        function setNext(n: raw_ptr<Node>) {
            this.next = n;
        }

        function getNext() raw_ptr<Node> {
            return this.next;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var head = allocator.create<Node>(100);
        head.setNext(createNode(allocator, 200));

        // Modify tail through head's next pointer
        head.getNext().setValue(999);

        // Access through the chain (not through moved var)
        return head.getNext().getValue();
    }
  )");
  EXPECT_EQ(value, 999);
}

TEST(LinkedListTest, access_deep_chain) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        function init(v: i32) {
            this.value = v;
        }

        function getValue() i32 {
            return this.value;
        }

        function setNext(n: raw_ptr<Node>) {
            this.next = n;
        }

        function getNext() raw_ptr<Node> {
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
        a.setNext(createNode(allocator, 2));
        a.getNext().setNext(createNode(allocator, 3));
        a.getNext().getNext().setNext(createNode(allocator, 4));

        // Access 4th element: a -> b -> c -> d
        return a.getNext().getNext().getNext().getValue();
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(LinkedListTest, null_terminated_list) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        function init(v: i32) {
            this.value = v;
            this.next = null;
        }

        function getValue() i32 {
            return this.value;
        }

        function setNext(n: raw_ptr<Node>) {
            this.next = n;
        }

        function getNext() raw_ptr<Node> {
            return this.next;
        }

        function hasNext() bool {
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
        n1.setNext(createNode(allocator, 20));
        n1.getNext().setNext(createNode(allocator, 30));
        // n3.next is null by default from init

        // Sum all values by checking for null
        var sum = n1.getValue();
        if (n1.hasNext()) {
            sum = sum + n1.getNext().getValue();
            if (n1.getNext().hasNext()) {
                sum = sum + n1.getNext().getNext().getValue();
            }
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 60);
}

TEST(LinkedListTest, while_loop_traversal) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        function init(v: i32) {
            this.value = v;
            this.next = null;
        }

        function getValue() i32 {
            return this.value;
        }

        function setNext(n: raw_ptr<Node>) {
            this.next = n;
        }

        function getNext() raw_ptr<Node> {
            return this.next;
        }

        function hasNext() bool {
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
        head.setNext(createNode(allocator, 2));
        head.getNext().setNext(createNode(allocator, 3));
        head.getNext().getNext().setNext(createNode(allocator, 4));
        head.getNext().getNext().getNext().setNext(createNode(allocator, 5));

        // Traverse with while loop
        var sum = 0;
        var curr: raw_ptr<Node> = head;
        while (curr != null) {
            sum = sum + curr.getValue();
            curr = curr.getNext();
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(LinkedListTest, list_class_with_methods) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    class Node {
        var value: i32;
        var next: raw_ptr<Node>;

        function init(v: i32) {
            this.value = v;
            this.next = null;
        }

        function getValue() i32 {
            return this.value;
        }

        function setNext(n: raw_ptr<Node>) {
            this.next = n;
        }

        function getNext() raw_ptr<Node> {
            return this.next;
        }

        function hasNext() bool {
            return this.next != null;
        }
    }

    function createNode(alloc: ref HeapAllocator, v: i32) raw_ptr<Node> {
        return alloc.create<Node>(v);
    }

    class List {
        var head: raw_ptr<Node>;
        var tail: raw_ptr<Node>;

        function init() {
            this.head = null;
            this.tail = null;
        }

        function append(alloc: ref HeapAllocator, v: i32) {
            if (this.tail != null) {
                // Append to existing list - create node and link via tail
                this.tail.setNext(createNode(alloc, v));
                this.tail = this.tail.getNext();
            } else {
                // First node - set both head and tail
                this.head = createNode(alloc, v);
                this.tail = this.head;
            }
        }

        function sum() i32 {
            var total = 0;
            var curr: raw_ptr<Node> = this.head;
            while (curr != null) {
                total = total + curr.getValue();
                curr = curr.getNext();
            }
            return total;
        }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var list = allocator.create<List>();
        list.append(allocator, 1);
        list.append(allocator, 2);
        list.append(allocator, 3);
        list.append(allocator, 4);
        list.append(allocator, 5);
        return list.sum();
    }
  )");
  EXPECT_EQ(value, 15);
}