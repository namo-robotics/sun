// tests/functions/generic/test_container_signatures.cpp - Generic functions
// whose signatures name a container that mentions itself
//
// `Vec<T> implements IIterable<ref T, Vec<T>>` names Vec inside Vec, and a
// generic function that takes, returns or builds one asks the compiler to
// resolve that shape while T is still a type parameter. Working out the shape
// twice used to read Vec's `T` in IIterator's bindings, where `T` is a
// different parameter that happens to share the name, and the two never
// settled (issue #144).

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// ============================================================================
// Stdlib containers in a generic function's signature
// ============================================================================

TEST(Functions_Generic_ContainerSignatures, vec_parameter) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function count<T>(v: ref Vec<T>) i64 {
        return v.size();
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i64>(allocator, 4);
        v.push(10);
        v.push(20);
        return count<i64>(v);
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Functions_Generic_ContainerSignatures, vec_return_type_and_local) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function build<T>(allocator: ref HeapAllocator, first: T) Vec<T> {
        var v = Vec<T>(allocator, 2);
        v.push(first);
        return v;
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var v = build<i64>(allocator, 7);
        return v.get(0);
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Functions_Generic_ContainerSignatures, linked_list_parameter) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function count<T>(l: ref LinkedList<T>) i64 {
        return l.size();
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var l = LinkedList<i32>(allocator);
        l.push_back(3);
        l.push_back(4);
        l.push_back(5);
        return count<i32>(l);
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Functions_Generic_ContainerSignatures, map_parameter_two_type_params) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function count<K, V>(m: ref Map<K, V>) i64 {
        return m.size();
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Map<i32, i32>(allocator, 8);
        m.insert(1, 10);
        m.insert(2, 20);
        return count<i32, i32>(m);
    }
  )");
  EXPECT_EQ(value, 2);
}

// The template's own shape is resolved even when nothing ever calls it, so
// the signature alone has to be enough.
TEST(Functions_Generic_ContainerSignatures, uncalled_vec_signature_resolves) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function never_called<T>(v: ref Vec<T>) i32 {
        return 0;
    }

    function main() i32 {
        return 5;
    }
  )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// The same shape without the stdlib
// ============================================================================

// `Cell<T> implements IPeekable<ref T, Cell<T>>` is IIterable's shape written
// out: the class is a type argument of the interface it implements, and the
// interface's own first parameter is also called T. Nothing here comes from a
// bundle, so a regression cannot hide behind the stdlib.
TEST(Functions_Generic_ContainerSignatures, self_referential_interface) {
  auto value = executeString(R"(
    interface IPeeker<T, Container> {
        method peek(container: ref Container) T;
    }

    interface IPeekable<T, Self> {
        method peeker() IPeeker<T, Self>;
    }

    class CellPeeker<T> implements IPeeker<ref T, Cell<T>> {
        init() {}
        method peek(container: ref Cell<T>) ref T {
            return container.value;
        }
    }

    class Cell<T> implements IPeekable<ref T, Cell<T>> {
        var value: T;
        init(v: T) { this.value = v; }
        method peeker() IPeeker<ref T, Cell<T>> {
            return CellPeeker<T>();
        }
    }

    function read<T>(c: ref Cell<T>) T {
        return c.value;
    }

    function main() i32 {
        var c = Cell<i32>(7);
        return read<i32>(c);
    }
  )");
  EXPECT_EQ(value, 7);
}
