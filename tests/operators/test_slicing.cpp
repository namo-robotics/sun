// tests/operators/test_slicing.cpp - The slice operator on classes
//
// `x[a:b]` on a class value calls its `__slice__` method with one
// std.SliceRange per dimension (codegen/expressions/arrays.cpp). A blank
// bound arrives as has_start / has_end false rather than as a number, and a
// class without `__slice__` cannot be sliced at all. SliceRange is a stdlib
// class, so these programs load it.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Operators_Slicing, slice_syntax_calls_the_class_slice_method) {
  auto value = executeStringWithStdlib(R"(
    using std;

    class Row {
        init() {}
        // Encodes the bounds it was handed as start * 10 + end
        public const method __slice__(ranges: const ref array<SliceRange>) i64 {
            return ranges[0].start * 10 + ranges[0].end;
        }
    }

    function main() i64 {
        var r = Row();
        return r[2:5];
    }
  )");
  EXPECT_EQ(value, 25);
}

TEST(Operators_Slicing, open_ended_slices_report_missing_bounds) {
  auto value = executeStringWithStdlib(R"(
    using std;

    class Row {
        init() {}
        // Bit 1 says a start was written, bit 2 says an end was
        public const method __slice__(ranges: const ref array<SliceRange>) i64 {
            var bits: i64 = 0;
            if (ranges[0].has_start) { bits = bits + 1; }
            if (ranges[0].has_end) { bits = bits + 2; }
            return bits;
        }
    }

    function main() i64 {
        var r = Row();
        if (r[1:] != 1) { return 10; }
        if (r[:4] != 2) { return 20; }
        if (r[:] != 0) { return 30; }
        if (r[1:4] != 3) { return 40; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Operators_Slicing, slicing_a_class_without_slice_method_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;

    class Row {
        init() {}
    }

    function main() i64 {
        var r = Row();
        var part = r[1:4];
        return 0;
    }
  )"),
                                "does not implement __slice__");
}
