// tests/stdlib/collections/test_linked_list.cpp - Disabled memory-safety UB
// demonstrations for stdlib LinkedList<T>. The behavior tests now live in
// stdlib/linked_list_tests.sun.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Memory Safety - known UB scenarios (borrow checker limitations)
// ============================================================================

// Use-after-free: stale raw_ptr to a node survives pop_front freeing it.
// The borrow checker does NOT track raw_ptr lifetimes, so this compiles
// and executes without error — but dereferences freed memory (UB).
TEST(Stdlib_Collections_LinkedList, DISABLED_use_after_free_stale_raw_ptr) {
  // This test demonstrates UB that the borrow checker cannot catch.
  // The stale raw_ptr reads freed memory. In practice this may return
  // the old value (memory not yet reclaimed) or garbage.
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var allocator = make_heap_allocator();
        var ll = LinkedList<i32>(allocator);
        ll.push_back(42);

        // Alias the internal head pointer
        var stale: raw_ptr<LinkedListNode<i32>> = ll.head;

        // Free the node via pop
        try { var v = ll.pop_front(); } catch (e: IError) { return -1; }

        // UB: stale points to freed memory
        return stale.get_value();
    }
  )");
  // UB: may return 42 (stale read) or anything else
  (void)value;
}

// Double-free: passing a LinkedList by value into a struct copies the
// raw_ptr fields (head/tail). Both the original and the copy call deinit,
// freeing the same nodes twice. The borrow checker does NOT prevent this.
TEST(Stdlib_Collections_LinkedList, DISABLED_double_free_via_copy) {
  // This test crashes with "free(): invalid pointer" — confirmed double-free.
  // Disabled because it aborts the test process.
  auto value = executeStringWithStdlib(R"(
    using std;

    class ListWrapper {
        var inner: LinkedList<i32>;

        init(list: LinkedList<i32>) {
            this.inner = list;
        }

        method getSize() i64 {
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
