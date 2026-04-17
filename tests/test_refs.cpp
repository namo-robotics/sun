// tests/test_refs.cpp

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Reference Tests
// ============================================================================

TEST(RefTest, basic_reference_read) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 42;
        ref r = x;
        return r;
    };
  )");
  EXPECT_EQ(value, 42);
}

TEST(RefTest, reference_modify_original) {
  // Modifying original variable is visible through reference
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        x = 20;
        return r;
    };
  )");
  EXPECT_EQ(value, 20);
}

TEST(RefTest, reference_modify_through_ref) {
  // Modifying through reference changes the original
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        r = 30;
        return x;
    };
  )");
  EXPECT_EQ(value, 30);
}

// ============================================================================
// Reference Arithmetic
// ============================================================================

TEST(RefTest, reference_in_arithmetic) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 5;
        ref r = x;
        return r * 2 + 3;
    };
  )");
  EXPECT_EQ(value, 13);
}

TEST(RefTest, reference_compound_expression) {
  auto value = executeString(R"(
    function main() i32 {
        var a: i32 = 10;
        var b: i32 = 20;
        ref ra = a;
        ref rb = b;
        return ra + rb;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(RefTest, reference_increment) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 5;
        ref r = x;
        r = r + 1;
        return x;
    };
  )");
  EXPECT_EQ(value, 6);
}

// ============================================================================
// Reference to Global Variables
// ============================================================================

TEST(RefTest, reference_to_global) {
  auto value = executeString(R"(
    var g: i32 = 100;

    function main() i32 {
        ref r = g;
        r = 200;
        return g;
    };
  )");
  EXPECT_EQ(value, 200);
}

TEST(RefTest, reference_read_global) {
  auto value = executeString(R"(
    var g: i32 = 77;

    function main() i32 {
        ref r = g;
        return r;
    };
  )");
  EXPECT_EQ(value, 77);
}

// ============================================================================
// Multiple References
// ============================================================================

TEST(RefTest, sequential_refs_to_same_var) {
  // Sequential (non-overlapping) refs to the same variable are allowed
  // Each ref must go out of scope before the next one is created
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        if (true) {
            ref r1 = x;
            r1 = 50;
        };
        // r1 is out of scope, we can create a new ref
        if (true) {
            ref r2 = x;
            return r2;
        };
        return x;
    };
  )");
  EXPECT_EQ(value, 50);
}

TEST(RefTest, refs_to_different_vars) {
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

// ============================================================================
// References with Different Types
// ============================================================================

TEST(RefTest, reference_f64) {
  auto value = executeString(R"(
    function main() f64 {
        var x: f64 = 3.14;
        ref r = x;
        return r;
    };
  )");
  EXPECT_TRUE(std::holds_alternative<double>(value));
  EXPECT_NEAR(std::get<double>(value), 3.14, 0.001);
}

TEST(RefTest, reference_modify_f64) {
  auto value = executeString(R"(
    function main() f64 {
        var x: f64 = 1.5;
        ref r = x;
        r = 2.5;
        return x;
    };
  )");
  EXPECT_TRUE(std::holds_alternative<double>(value));
  EXPECT_NEAR(std::get<double>(value), 2.5, 0.001);
}

TEST(RefTest, reference_bool) {
  auto value = executeString(R"(
    function main() i32 {
        var flag: bool = true;
        ref r = flag;
        if (r) {
            return 1;
        };
        return 0;
    };
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// References in Control Flow
// ============================================================================

TEST(RefTest, reference_in_if_condition) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        if (r > 5) {
            return 1;
        };
        return 0;
    };
  )");
  EXPECT_EQ(value, 1);
}

TEST(RefTest, reference_modified_in_if) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 0;
        ref r = x;
        if (true) {
            r = 42;
        };
        return x;
    };
  )");
  EXPECT_EQ(value, 42);
}

TEST(RefTest, reference_in_while_loop) {
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        ref r = count;
        var i: i32 = 0;
        while (i < 5) {
            r = r + 1;
            i = i + 1;
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// References in Functions
// ============================================================================

TEST(RefTest, reference_before_function_call) {
  auto value = executeString(R"(
    function add(a: i32, b: i32) i32 {
        return a + b;
    };

    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        return add(r, 5);
    };
  )");
  EXPECT_EQ(value, 15);
}

TEST(RefTest, reference_with_function_result) {
  auto value = executeString(R"(
    function compute() i32 {
        return 100;
    };

    function main() i32 {
        var x: i32 = 0;
        ref r = x;
        r = compute();
        return x;
    };
  )");
  EXPECT_EQ(value, 100);
}

// ============================================================================
// Reference Edge Cases
// ============================================================================

TEST(RefTest, reference_zero_value) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 0;
        ref r = x;
        return r;
    };
  )");
  EXPECT_EQ(value, 0);
}

TEST(RefTest, reference_negative_value) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = -42;
        ref r = x;
        return r;
    };
  )");
  EXPECT_EQ(value, -42);
}

TEST(RefTest, reference_chain_modifications) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        ref r = x;
        r = r + 1;
        r = r * 2;
        r = r + 3;
        return x;
    };
  )");
  EXPECT_EQ(value, 7);  // ((1+1)*2)+3 = 7
}

// ============================================================================
// References with Comparisons
// ============================================================================

TEST(RefTest, reference_equality_comparison) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 42;
        ref r = x;
        if (r == 42) {
            return 1;
        };
        return 0;
    };
  )");
  EXPECT_EQ(value, 1);
}

TEST(RefTest, reference_less_than_comparison) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 5;
        ref r = x;
        if (r < 10) {
            return 1;
        };
        return 0;
    };
  )");
  EXPECT_EQ(value, 1);
}

TEST(RefTest, two_refs_comparison) {
  auto value = executeString(R"(
    function main() i32 {
        var a: i32 = 10;
        var b: i32 = 20;
        ref ra = a;
        ref rb = b;
        if (ra < rb) {
            return 1;
        };
        return 0;
    };
  )");
  EXPECT_EQ(value, 1);
}

TEST(RefTest, i32_ref_arg) {
  auto value = executeString(R"(
    function foo(x: ref i32) {
        x = 100;
    };
    function main() i32 {
        var x: i32 = 0;
        foo(x);
        return x;
    };
  )");
  EXPECT_EQ(value, 100);
}

TEST(RefTest, pass_i32_by_ref) {
  auto value = executeString(R"(
    function foo(x: ref i32) {
        x = 100;
    };
    function main() i32 {
        var x: i32 = 0;
        foo(x);
        return x;
    };
  )");
  EXPECT_EQ(value, 100);
}
