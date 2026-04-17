// tests/test_borrow_checker.cpp
// Tests for the Rust-style borrow checker

#include <gtest/gtest.h>

#include "borrow_checker/borrow_checker.h"
#include "execution_utils.h"

// ============================================================================
// Valid Borrow Patterns - Should Compile Successfully
// ============================================================================

TEST(BorrowCheckerTest, single_mutable_ref) {
  // Single mutable reference is allowed
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        r = 50;
        return x;
    };
  )");
  EXPECT_EQ(value, 50);
}

TEST(BorrowCheckerTest, ref_goes_out_of_scope) {
  // Reference going out of scope frees the borrow
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        if (true) {
            ref r = x;
            r = 20;
        };
        // x is no longer borrowed here
        x = 30;
        return x;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(BorrowCheckerTest, sequential_refs_to_same_var) {
  // Sequential (non-overlapping) refs to same variable are allowed
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        if (true) {
            ref r1 = x;
            r1 = 20;
        };
        if (true) {
            ref r2 = x;
            r2 = 30;
        };
        return x;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(BorrowCheckerTest, refs_to_different_vars) {
  // Multiple refs to different variables are allowed
  auto value = executeString(R"(
    function main() i32 {
        var a: i32 = 5;
        var b: i32 = 10;
        ref ra = a;
        ref rb = b;
        ra = ra + rb;
        return a;
    };
  )");
  EXPECT_EQ(value, 15);
}

TEST(BorrowCheckerTest, ref_passed_to_function) {
  // Passing a ref to a function works
  auto value = executeString(R"(
    function increment(x: ref i32) {
        x = x + 1;
    };

    function main() i32 {
        var val: i32 = 10;
        increment(val);
        return val;
    };
  )");
  EXPECT_EQ(value, 11);
}

TEST(BorrowCheckerTest, nested_function_with_ref_param) {
  // Function taking ref parameter, called from main
  auto value = executeString(R"(
    function double_it(x: ref i32) {
        x = x * 2;
    };

    function main() i32 {
        var n: i32 = 7;
        double_it(n);
        return n;
    };
  )");
  EXPECT_EQ(value, 14);
}

// ============================================================================
// Borrow Violations - Should Be Caught by Borrow Checker
// ============================================================================

TEST(BorrowCheckerTest, double_mutable_borrow_is_error) {
  // Two simultaneous mutable borrows of the same variable is an error
  EXPECT_THROW(executeString(R"(
        function main() i32 {
            var x: i32 = 10;
            ref r1 = x;
            ref r2 = x;
            r1 = 50;
            return r2;
        };
      )"),
               SunError);
}

// ============================================================================
// Return Type Restrictions - References Cannot Be Returned
// ============================================================================

TEST(BorrowCheckerTest, function_returns_value_not_ref) {
  // Returning the VALUE through a ref is fine (value is copied)
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 42;
        ref r = x;
        return r;  // Returns the value, not the reference
    };
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Class/Struct Field Restrictions
// ============================================================================

TEST(BorrowCheckerTest, class_without_ref_fields) {
  // Classes can have regular fields
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        };
        
        function sum() i32 {
            return this.x + this.y;
        };
    };

    function main() i32 {
        var p = Point(3, 4);
        return p.sum();
    };
  )");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Scope-Based Borrow Invalidation
// ============================================================================

TEST(BorrowCheckerTest, borrow_ends_at_scope_exit) {
  // Borrow should end when the reference goes out of scope
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 100;
        if (true) {
            ref r = x;
            r = 200;
        };
        // r is out of scope, x is no longer borrowed
        return x;
    };
  )");
  EXPECT_EQ(value, 200);
}

TEST(BorrowCheckerTest, while_loop_borrow) {
  // Borrow inside while loop should be scoped properly
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        var i: i32 = 0;
        while (i < 3) {
            ref r = count;
            r = r + 10;
            i = i + 1;
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(BorrowCheckerTest, error_on_string_use_after_move) {
  EXPECT_THROW(executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function consume(s: ref String) {
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var s1 = String(allocator, "Hello World");
        var s2 = s1;
        consume(s1);
        return 0;
    };
  )"),
               std::exception);
}

// ============================================================================
// Move Semantics for By-Value Passing
// ============================================================================

// When REQUIRE_REF_FOR_COMPOUND_PARAMS is false, classes can be passed by value
// and the source is automatically moved (cannot be used afterward)
TEST(BorrowCheckerTest, use_after_move_on_byval_pass) {
  // Passing a class by value should move it - using the source after is an
  // error
  EXPECT_THROW(executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        }
    }

    function consume(p: Point) i32 {
        return p.x + p.y;
    }

    function main() i32 {
        var p = Point(3, 4);
        var result = consume(p);  // p is moved here
        return p.x;  // ERROR: use of moved variable 'p'
    }
  )"),
               std::exception);
}

TEST(BorrowCheckerTest, byval_pass_moves_ownership) {
  // Passing by value should work - moved value is consumed by function
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init(n: i32) {
            this.count = n;
        }
    }

    function sum_counters(a: Counter, b: Counter) i32 {
        return a.count + b.count;
    }

    function main() i32 {
        var c1 = Counter(10);
        var c2 = Counter(20);
        return sum_counters(c1, c2);  // Both are moved, result is 30
    }
  )");
  EXPECT_EQ(value, 30);
}