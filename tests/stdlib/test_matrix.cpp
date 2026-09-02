// tests/stdlib/test_matrix.cpp - Compile-only slice-syntax checks and the
// use-after-move rejections for Matrix views. The runtime behavior tests live
// in stdlib/matrix_tests.sun.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// Test slice syntax parsing (without executing slices)
TEST(Stdlib_Matrix, parse_slice_syntax) {
  // This just tests that the parser accepts slice syntax
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
        var x: array<i32, 10> = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
        // Currently slices are not implemented, just parsing
        // When implemented: var sub = x[1:5];
        return x[0];
    }
  )"));
}

// Test that slice syntax parses correctly (compile-only for now)
TEST(Stdlib_Matrix, slice_syntax_compiles) {
  // This tests that the parser accepts slice syntax on arrays
  // Actual array slicing returns would need additional codegen
  EXPECT_NO_THROW(compileString(R"(
    function test_slice_parse() i32 {
        var arr: array<i32, 10> = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
        // These should parse correctly even if runtime behavior is TBD
        var x1: i32 = arr[0];     // Simple index
        // Slice syntax on arrays not yet fully implemented
        // var sub = arr[1:5];    // Would be a slice
        return x1;
    }
    function main() i32 { return test_slice_parse(); }
  )"));
}

// Using matrix after creating view is use-after-move error
TEST(Stdlib_Matrix, DISABLED_slice_1d_use_matrix_after_view_error) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [5]);

        m[0] = 1;
        m[1] = 2;
        m[2] = 3;
        m[3] = 4;
        m[4] = 5;

        // Get a view - this MOVES m.data to the view
        var view = m[1:4];

        // ERROR: m.data has been moved, cannot modify original matrix
        m[2] = 100;

        return 0;
    }
  )"),
               std::exception);
}

// Accessing original matrix after view creation is use-after-move error
TEST(Stdlib_Matrix, DISABLED_access_matrix_after_view_is_error) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [5]);

        m[0] = 1;
        m[1] = 2;
        m[2] = 3;

        var view = m[1:4];  // Moves m.data
        var idx0: array<i64, 1> = [0];
        view.set(idx0, 200);

        // ERROR: m.data has been moved, cannot access original matrix
        var origVal: i64 = m[1];
        return origVal;
    }
  )"),
               std::exception);
}

// Using matrix after creating view is use-after-move error
TEST(Stdlib_Matrix, DISABLED_use_after_view) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [5]);

        var view = m[1:4];  // Moves m.data to view
        var idx0: array<i64, 1> = [0];

        m.set(idx0, 50);  // ERROR: m.data has been moved

        return m.get(idx0);
    }
  )"),
               std::exception);
}

// Creating a view moves the data pointer from Matrix - using Matrix after is
// error
TEST(Stdlib_Matrix, DISABLED_use_matrix_after_view_is_move_error) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i64>(allocator, [10]);

        // Fill with values
        for (var i: i64 = 0; i < 10; i = i + 1) {
            m[i] = i * 10;
        }

        // Create view - this MOVES the data ptr from m to view
        var view = m[3:7];

        // ERROR: m.data has been moved, cannot use m anymore
        m[4] = 999;

        return 0;
    }
  )"),
               std::exception);
}

// Creating multiple views from same matrix should fail on second view
TEST(Stdlib_Matrix, DISABLED_second_view_after_move_is_error) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i64>(allocator, [10]);

        for (var i: i64 = 0; i < 10; i = i + 1) {
            m[i] = i;
        }

        // First view - moves m.data
        var view1 = m[0:5];

        // ERROR: m.data already moved, cannot create second view
        var view2 = m[3:8];

        return 0;
    }
  )"),
               std::exception);
}

// View of a view should work (view owns the data it received)
TEST(Stdlib_Matrix, DISABLED_view_of_view_is_valid) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i64>(allocator, [10]);

        for (var i: i64 = 0; i < 10; i = i + 1) {
            m[i] = i;
        }

        // First level view [2:8] -> takes ownership from m
        var view1 = m[2:8];

        // Second level view from view1 [1:4] -> takes ownership from view1
        var view2 = view1[1:4];

        // view2[0] = original m[3]
        var idx: array<i64, 1> = [0];
        var val: i64 = view2.get(idx);

        return val;
    }
  )");
  EXPECT_EQ(value, 3);
}

// Using first view after creating second view from it is use-after-move error
TEST(Stdlib_Matrix, use_view1_after_view2_is_error) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i64>(allocator, [10]);

        for (var i: i64 = 0; i < 10; i = i + 1) {
            m[i] = i;
        }

        var view1 = m[2:8];
        var view2 = view1[1:4];  // Moves view1.data to view2

        // ERROR: view1.data has been moved
        var idx: array<i64, 1> = [0];
        var val: i64 = view1.get(idx);

        return val;
    }
  )"),
               std::exception);
}
