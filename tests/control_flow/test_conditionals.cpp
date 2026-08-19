// tests/control_flow/test_conditionals.cpp - if / else if / else
//
// Covers: branch selection, else-if chains, nesting, branch-local scoping,
// what counts as a condition, and returning a class out of a branch.
// Conditions built from && / || live in Operators_Logical.

#include <gtest/gtest.h>

#include "execution_utils.h"

TEST(ControlFlow_Conditionals, true_branch_taken) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 5;
        if (x > 3) {
            return 1;
        }
        return 2;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(ControlFlow_Conditionals, false_condition_falls_through) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 1;
        if (x > 3) {
            return 1;
        }
        return 2;
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(ControlFlow_Conditionals, if_else_selects_branch) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 1;
        if (x > 3) {
            return 10;
        } else {
            return 20;
        }
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(ControlFlow_Conditionals, else_if_chain_picks_first_match) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 5;
        if (x > 10) {
            return 1;
        } else if (x > 3) {
            return 2;
        } else {
            return 3;
        }
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(ControlFlow_Conditionals, else_if_chain_falls_through_to_else) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 0;
        if (x > 10) {
            return 1;
        } else if (x > 3) {
            return 2;
        } else {
            return 3;
        }
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(ControlFlow_Conditionals, nested_if) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 5;
        if (x > 1) {
            if (x > 4) {
                return 9;
            }
            return 8;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(ControlFlow_Conditionals, branch_body_runs_without_returning) {
  auto value = executeString(R"(
    function main() i32 {
        var total = 0;
        if (true) {
            total = total + 3;
        }
        if (false) {
            total = total + 100;
        }
        return total;
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(ControlFlow_Conditionals, bool_variable_as_condition) {
  auto value = executeString(R"(
    function main() i32 {
        var flag: bool = true;
        if (flag) {
            return 42;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ControlFlow_Conditionals, nonzero_integer_condition_is_truthy) {
  // Sun does not require a bool: a non-zero integer condition is taken.
  auto value = executeString(R"(
    function main() i32 {
        if (1) {
            return 7;
        }
        return 3;
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(ControlFlow_Conditionals, zero_integer_condition_is_falsy) {
  auto value = executeString(R"(
    function main() i32 {
        if (0) {
            return 7;
        }
        return 3;
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(ControlFlow_Conditionals, float_comparison_as_condition) {
  auto value = executeString(R"(
    function main() i32 {
        var x: f64 = 2.5;
        if (x < 3.0) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(ControlFlow_Conditionals, variable_declared_in_branch_is_scoped_to_it) {
  EXPECT_THROW(executeString(R"(
        function main() i32 {
            var a = 1;
            if (a == 1) {
                var b = 7;
                a = a + b;
            }
            return b;
        }
      )"),
               SunError);
}

TEST(ControlFlow_Conditionals, branch_shadows_outer_variable) {
  auto value = executeString(R"(
    function main() i32 {
        var x = 1;
        if (true) {
            var x = 50;
            x = x + 1;
        }
        return x;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(ControlFlow_Conditionals, branch_returns_class_by_value) {
  auto value = executeString(R"(
    class Box {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function get() i32 { return this.v; }
    }

    function pick(n: i32) Box {
        if (n > 0) {
            return Box(n);
        } else {
            return Box(0 - n);
        }
    }

    function main() i32 {
        var b = pick(0 - 5);
        return b.get();
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(ControlFlow_Conditionals, condition_calls_a_function) {
  auto value = executeString(R"(
    function is_big(n: i32) bool {
        return n > 100;
    }

    function main() i32 {
        if (is_big(500)) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}
