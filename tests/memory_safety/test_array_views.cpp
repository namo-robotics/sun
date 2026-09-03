// tests/memory_safety/test_array_views.cpp - An unsized array<T> is a view
//
// An unsized array is a view of some sized array with its rank erased. It may
// only be written behind `ref`, and a `ref array<T>` field or return follows
// the ordinary reference rules: the holder is a borrow, and a returned view
// borrows the call's inputs.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {

const char* kViewRule = "may only be used behind ref";

}  // namespace

TEST(MemorySafety_ArrayViews, bare_unsized_field_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Holder {
        var xs: array<i64>;
        init(xs: const ref array<i64>) { this.xs = xs; }
    }
    function main() i32 { return 0; }
  )"),
                                kViewRule);
}

TEST(MemorySafety_ArrayViews, bare_unsized_local_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
        var xs: array<i64> = [1, 2, 3];
        return 0;
    }
  )"),
                                kViewRule);
}

TEST(MemorySafety_ArrayViews, bare_unsized_param_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function total(xs: array<i64>) i64 { return xs[0]; }
    function main() i32 { return 0; }
  )"),
                                kViewRule);
}

TEST(MemorySafety_ArrayViews, bare_unsized_return_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function first(xs: const ref array<i64>) array<i64> { return xs; }
    function main() i32 { return 0; }
  )"),
                                kViewRule);
}

TEST(MemorySafety_ArrayViews, bare_unsized_global_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    var g: array<i32> = [1];
    function main() i32 { return 0; }
  )"),
                                kViewRule);
}

TEST(MemorySafety_ArrayViews, bare_unsized_generic_argument_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;
    function count(rows: const ref Vec<array<i64>>) i64 { return rows.size(); }
    function main() i32 { return 0; }
  )"),
                                kViewRule);
}

// A `ref array<T>` field is a reference field: the class becomes a holder
// that borrows the array it views and cannot leave the frame.
TEST(MemorySafety_ArrayViews, ref_view_field_holder_cannot_be_returned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Holder {
        var xs: const ref array<i64>;
        init(xs: const ref array<i64>) { this.xs = xs; }
        public const method get(i: i64) i64 { return this.xs[i]; }
    }
    function make() Holder {
        var local: array<i64, 3> = [10, 20, 30];
        return Holder(local);
    }
    function main() i32 { return 0; }
  )"),
                                "cannot return a value that stores references");
}

TEST(MemorySafety_ArrayViews, ref_view_field_reads_the_viewed_array) {
  auto value = executeString(R"(
    class Holder {
        var xs: const ref array<i64>;
        init(xs: const ref array<i64>) { this.xs = xs; }
        public const method get(i: i64) i64 { return this.xs[i]; }
        public const method rank() i64 { return this.xs.ndims(); }
    }
    function main() i32 {
        var a: array<i64, 3> = [10, 20, 30];
        var h = Holder(a);
        return _convert<i32>(h.get(1) + h.rank());
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(MemorySafety_ArrayViews, ref_view_field_holder_borrows_the_array) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Holder {
        var xs: const ref array<i64>;
        init(xs: const ref array<i64>) { this.xs = xs; }
        public const method get(i: i64) i64 { return this.xs[i]; }
    }
    function main() i32 {
        var a: array<i64, 3> = [10, 20, 30];
        var h = Holder(a);
        var b = a;
        return _convert<i32>(h.get(1));
    }
  )"),
                                "Borrow check failed");
}

// A `ref array<T>` return is a reference return: it may hand back a view
// of its inputs, never of its own frame.
TEST(MemorySafety_ArrayViews, ref_view_return_passes_through_a_borrow) {
  auto value = executeString(R"(
    function same(xs: const ref array<i64>) const ref array<i64> { return xs; }
    function main() i32 {
        var a: array<i64, 3> = [10, 20, 30];
        var v = same(a);
        return _convert<i32>(v[2] + v.dim(0));
    }
  )");
  EXPECT_EQ(value, 33);
}

TEST(MemorySafety_ArrayViews, ref_view_return_of_local_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function make() const ref array<i64> {
        var local: array<i64, 3> = [10, 20, 30];
        return local;
    }
    function main() i32 { return 0; }
  )"),
                                "Borrow check failed");
}

// A view sees writes made through the array it views.
TEST(MemorySafety_ArrayViews, writes_through_a_view_reach_the_array) {
  auto value = executeString(R"(
    function bump(xs: ref array<i32>) void {
        xs[1] = xs[1] + 100;
    }
    function main() i32 {
        var a: array<i32, 3> = [1, 2, 3];
        bump(a);
        return a[1];
    }
  )");
  EXPECT_EQ(value, 102);
}
