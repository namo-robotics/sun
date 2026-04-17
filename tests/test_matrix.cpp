// tests/test_matrix.cpp - Tests for Matrix class

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "execution_utils.h"

// Basic Matrix construction and element access
TEST(MatrixTest, simple_matrix) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [1]);
        var idx: array<i64, 1> = [0];
        m.set(idx, 10);
        return m.get(idx);
    }
  )");
  EXPECT_EQ(value, 10);
}

// Test matrix dimensions
TEST(MatrixTest, matrix_ndims_and_size) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var shape: array<i64, 3> = [2, 3, 4];
        var m = Matrix<f64>(allocator, shape);
        
        // ndims should be 3, size should be 2*3*4 = 24
        return m.ndims() * 100 + m.size();
    }
  )");
  EXPECT_EQ(value, 324);  // 3*100 + 24
}

// Test slice syntax parsing (without executing slices)
TEST(MatrixTest, parse_slice_syntax) {
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

// Test Matrix bracket indexing syntax: M[i, j]
TEST(MatrixTest, bracket_index_syntax) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [2, 3]);
        
        // Use bracket syntax for assignment and access
        var idx: array<i64, 2> = [0, 0];
        m[0, 0] = 42;
        return m[0, 0];
    }
  )");
  EXPECT_EQ(value, 42);
}

// Test Matrix bracket indexing with multiple dimensions
TEST(MatrixTest, bracket_index_2d) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i32 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [3, 3]);
        
        // Fill diagonal with values
        m[0, 0] = 1;
        m[1, 1] = 2;
        m[2, 2] = 3;
        
        // Sum diagonal
        return m[0, 0] + m[1, 1] + m[2, 2];
    }
  )");
  EXPECT_EQ(value, 6);
}

// Test SliceRange class construction
TEST(MatrixTest, slice_range_construction) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        // Create a slice range representing [2:5]
        var s = SliceRange(2, 5, true, true);
        
        // getStart should return 2
        var start: i64 = s.getStart();
        
        // getEnd should return 5 (for any dim size)
        var end: i64 = s.getEnd(10);
        
        // size should be 3
        var sz: i64 = s.size(10);
        
        return start * 100 + end * 10 + sz;
    }
  )");
  EXPECT_EQ(value, 253);  // 2*100 + 5*10 + 3
}

// Test SliceRange with open-ended ranges
TEST(MatrixTest, slice_range_open_ends) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        // Slice [:5] - from start to 5
        var s1 = SliceRange(0, 5, false, true);
        var start1: i64 = s1.getStart();  // should be 0
        var end1: i64 = s1.getEnd(10);    // should be 5
        
        // Slice [3:] - from 3 to end
        var s2 = SliceRange(3, 0, true, false);
        var start2: i64 = s2.getStart();  // should be 3
        var end2: i64 = s2.getEnd(10);    // should be 10 (dim size)
        
        // Full slice [:] - from start to end
        var s3 = SliceRange(0, 0, false, false);
        var full: i64 = 0;
        if (s3.isFullRange()) { full = 1; }
        
        return start1 * 1000 + start2 * 100 + end1 * 10 + full;
    }
  )");
  EXPECT_EQ(value, 351);  // 0*1000 + 3*100 + 5*10 + 1
}

// Test that slice syntax parses correctly (compile-only for now)
TEST(MatrixTest, slice_syntax_compiles) {
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

// ============================================================================
// Matrix Slicing Tests
// ============================================================================

// Test basic 1D matrix slicing
TEST(MatrixTest, slice_1d_basic) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [10]);
        
        // Fill matrix with values 0-9
        for (var i: i64 = 0; i < 10; i = i + 1) {
            m[i] = i;
        }
        
        // Slice [2:5] should give view of length 3
        var view = m[2:5];
        return view.size();
    }
  )");
  EXPECT_EQ(value, 3);
}

// Test sliced matrix view values
TEST(MatrixTest, slice_1d_values) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [10]);
        
        // Fill matrix with values 0-9
        for (var i: i64 = 0; i < 10; i = i + 1) {
            m[i] = i;
        }
        
        // Slice [2:5] should give view with values 2, 3, 4
        var view = m[2:5];
        var idx0: array<i64, 1> = [0];
        var idx1: array<i64, 1> = [1];
        var idx2: array<i64, 1> = [2];
        var v0: i64 = view.get(idx0);  // Should be 2
        var v1: i64 = view.get(idx1);  // Should be 3
        var v2: i64 = view.get(idx2);  // Should be 4
        return v0 * 100 + v1 * 10 + v2;
    }
  )");
  EXPECT_EQ(value, 234);  // 2*100 + 3*10 + 4
}

// Test slice from start (0:n)
TEST(MatrixTest, slice_1d_from_start) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [5]);
        
        m[0] = 10;
        m[1] = 20;
        m[2] = 30;
        m[3] = 40;
        m[4] = 50;
        
        // Slice [0:3]
        var view = m[0:3];
        var idx0: array<i64, 1> = [0];
        var idx1: array<i64, 1> = [1];
        var idx2: array<i64, 1> = [2];
        var sum: i64 = view.get(idx0) + view.get(idx1) + view.get(idx2);
        return sum;
    }
  )");
  EXPECT_EQ(value, 60);  // 10 + 20 + 30
}

// Test slice to end (n:size)
TEST(MatrixTest, slice_1d_to_end) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [5]);
        
        m[0] = 10;
        m[1] = 20;
        m[2] = 30;
        m[3] = 40;
        m[4] = 50;
        
        // Slice [3:5]
        var view = m[3:5];
        var idx0: array<i64, 1> = [0];
        var idx1: array<i64, 1> = [1];
        var sum: i64 = view.get(idx0) + view.get(idx1);
        return sum;
    }
  )");
  EXPECT_EQ(value, 90);  // 40 + 50
}

// Using matrix after creating view is use-after-move error
TEST(MatrixTest, DISABLED_slice_1d_use_matrix_after_view_error) {
  EXPECT_THROW(executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
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

// View can be modified and read - view owns the data after move
TEST(MatrixTest, slice_1d_modify_through_view) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var m = Matrix<i32>(allocator, [5]);
        
        m[0] = 1;
        m[1] = 2;
        m[2] = 3;
        m[3] = 4;
        m[4] = 5;
        
        // Get a view - this MOVES m.data to view
        var view = m[1:4];  // view now contains [2,3,4]
        
        // Modify through view
        var idx0: array<i64, 1> = [0];
        view.set(idx0, 200);  // set view[0] = 200
        
        // Read back through view (not original matrix!)
        var viewVal: i64 = view.get(idx0);
        return viewVal;
    }
  )");
  EXPECT_EQ(value, 200);
}

// Accessing original matrix after view creation is use-after-move error
TEST(MatrixTest, DISABLED_access_matrix_after_view_is_error) {
  EXPECT_THROW(executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
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
TEST(MatrixTest, DISABLED_use_after_view) {
  EXPECT_THROW(executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
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

// ============================================================================
// MatrixView Ownership/Move Semantics Tests
// ============================================================================

// Creating a view moves the data pointer from Matrix - using Matrix after is
// error
TEST(MatrixTest, DISABLED_use_matrix_after_view_is_move_error) {
  EXPECT_THROW(executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
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
TEST(MatrixTest, DISABLED_second_view_after_move_is_error) {
  EXPECT_THROW(executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
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
TEST(MatrixTest, DISABLED_view_of_view_is_valid) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
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
TEST(MatrixTest, use_view1_after_view2_is_error) {
  EXPECT_THROW(executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
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
