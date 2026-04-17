// tests/test_loops.cpp

#include <gtest/gtest.h>

#include "execution_utils.h"

// ============================================================================
// Basic For Loops
// ============================================================================

TEST(LoopTest, for_basic_sum) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 0; i < 5; i = i + 1) {
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 10);  // 0+1+2+3+4 = 10
}

TEST(LoopTest, for_step_two) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 0; i < 10; i = i + 2) {
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 20);  // 0+2+4+6+8 = 20
}

TEST(LoopTest, for_countdown) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 5; i > 0; i = i - 1) {
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 15);  // 5+4+3+2+1 = 15
}

TEST(LoopTest, for_no_iterations) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 42;
        for (var i: i32 = 10; i < 5; i = i + 1) {
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 42);  // Loop doesn't execute, sum unchanged
}

TEST(LoopTest, for_single_iteration) {
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        for (var i: i32 = 0; i < 1; i = i + 1) {
            count = count + 1;
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 1);
}

TEST(LoopTest, for_large_iteration) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 1; i <= 100; i = i + 1) {
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 5050);  // Sum of 1 to 100
}

// ============================================================================
// Basic While Loops
// ============================================================================

TEST(LoopTest, while_basic_sum) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        var i: i32 = 0;
        while (i < 5) {
            sum = sum + i;
            i = i + 1;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 10);  // 0+1+2+3+4 = 10
}

TEST(LoopTest, while_countdown) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        var i: i32 = 5;
        while (i > 0) {
            sum = sum + i;
            i = i - 1;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 15);  // 5+4+3+2+1 = 15
}

TEST(LoopTest, while_no_iterations) {
  auto value = executeString(R"(
    function main() i32 {
        var result: i32 = 100;
        while (false) {
            result = 0;
        };
        return result;
    };
  )");
  EXPECT_EQ(value, 100);  // Loop never executes
}

TEST(LoopTest, while_single_iteration) {
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        var done: bool = false;
        while (done == false) {
            count = count + 1;
            done = true;
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Nested Loops
// ============================================================================

TEST(LoopTest, nested_for_loops) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            for (var j: i32 = 0; j < 4; j = j + 1) {
                sum = sum + 1;
            };
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 12);  // 3 * 4 = 12
}

TEST(LoopTest, nested_while_loops) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        var i: i32 = 0;
        while (i < 3) {
            var j: i32 = 0;
            while (j < 4) {
                sum = sum + 1;
                j = j + 1;
            };
            i = i + 1;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 12);  // 3 * 4 = 12
}

TEST(LoopTest, nested_for_while) {
  auto value = executeString(R"(
    function main() i32 {
        var total: i32 = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            var j: i32 = 0;
            while (j < 2) {
                total = total + i + j;
                j = j + 1;
            };
        };
        return total;
    };
  )");
  // i=0: j=0,1 -> 0+0 + 0+1 = 1
  // i=1: j=0,1 -> 1+0 + 1+1 = 3
  // i=2: j=0,1 -> 2+0 + 2+1 = 5
  // Total: 1+3+5 = 9
  EXPECT_EQ(value, 9);
}

TEST(LoopTest, triple_nested_for) {
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        for (var i: i32 = 0; i < 2; i = i + 1) {
            for (var j: i32 = 0; j < 3; j = j + 1) {
                for (var k: i32 = 0; k < 4; k = k + 1) {
                    count = count + 1;
                };
            };
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 24);  // 2 * 3 * 4 = 24
}

// ============================================================================
// Loops with Variables
// ============================================================================

TEST(LoopTest, for_multiply_table) {
  auto value = executeString(R"(
    function main() i32 {
        var product: i32 = 1;
        for (var i: i32 = 1; i <= 5; i = i + 1) {
            product = product * i;
        };
        return product;
    };
  )");
  EXPECT_EQ(value, 120);  // 5! = 120
}

TEST(LoopTest, while_power_of_two) {
  auto value = executeString(R"(
    function main() i32 {
        var result: i32 = 1;
        var count: i32 = 0;
        while (count < 10) {
            result = result * 2;
            count = count + 1;
        };
        return result;
    };
  )");
  EXPECT_EQ(value, 1024);  // 2^10 = 1024
}

TEST(LoopTest, for_variable_bound) {
  auto value = executeString(R"(
    function main() i32 {
        var n: i32 = 5;
        var sum: i32 = 0;
        for (var i: i32 = 0; i < n; i = i + 1) {
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 10);  // 0+1+2+3+4 = 10
}

TEST(LoopTest, while_until_condition) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        while (x < 100) {
            x = x * 2;
        };
        return x;
    };
  )");
  EXPECT_EQ(value, 128);  // 1->2->4->8->16->32->64->128
}

// ============================================================================
// Loops with Functions
// ============================================================================

TEST(LoopTest, for_function_call_in_body) {
  auto value = executeString(R"(
    function square(x: i32) i32 {
        return x * x;
    };

    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 1; i <= 4; i = i + 1) {
            sum = sum + square(i);
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 30);  // 1 + 4 + 9 + 16 = 30
}

TEST(LoopTest, while_with_function_condition) {
  auto value = executeString(R"(
    function shouldContinue(x: i32) bool {
        return x < 5;
    };

    function main() i32 {
        var i: i32 = 0;
        var sum: i32 = 0;
        while (shouldContinue(i)) {
            sum = sum + i;
            i = i + 1;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 10);  // 0+1+2+3+4 = 10
}

TEST(LoopTest, loop_in_function) {
  auto value = executeString(R"(
    function sumTo(n: i32) i32 {
        var sum: i32 = 0;
        for (var i: i32 = 1; i <= n; i = i + 1) {
            sum = sum + i;
        };
        return sum;
    };

    function main() i32 {
        return sumTo(10);
    };
  )");
  EXPECT_EQ(value, 55);  // Sum of 1 to 10
}

TEST(LoopTest, recursive_vs_loop_factorial) {
  auto value = executeString(R"(
    function factorialLoop(n: i32) i32 {
        var result: i32 = 1;
        for (var i: i32 = 1; i <= n; i = i + 1) {
            result = result * i;
        };
        return result;
    };

    function factorialRecursive(n: i32) i32 {
        if (n <= 1) {
            return 1;
        } else {
            return n * factorialRecursive(n - 1);
        }
    };

    function main() i32 {
        var loopResult = factorialLoop(6);
        var recurResult = factorialRecursive(6);
        if (loopResult == recurResult) {
            return loopResult;
        } else {
            return -1;
        }
    };
  )");
  EXPECT_EQ(value, 720);  // 6! = 720
}

// ============================================================================
// Loops with Conditionals
// ============================================================================

TEST(LoopTest, for_with_if_inside) {
  auto value = executeString(R"(
    function main() i32 {
        var evenSum: i32 = 0;
        for (var i: i32 = 0; i < 10; i = i + 1) {
            if (i / 2 * 2 == i) {
                evenSum = evenSum + i;
            };
        };
        return evenSum;
    };
  )");
  EXPECT_EQ(value, 20);  // 0+2+4+6+8 = 20
}

TEST(LoopTest, while_with_early_exit_simulation) {
  auto value = executeString(R"(
    function main() i32 {
        var i: i32 = 0;
        var found: i32 = -1;
        var searching: bool = true;
        while (searching) {
            if (i * i > 50) {
                found = i;
                searching = false;
            } else {
                i = i + 1;
            };
        };
        return found;
    };
  )");
  EXPECT_EQ(value, 8);  // 8*8 = 64 > 50
}

TEST(LoopTest, nested_loop_with_condition) {
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        for (var i: i32 = 1; i <= 5; i = i + 1) {
            for (var j: i32 = 1; j <= 5; j = j + 1) {
                if (i <= j) {
                    count = count + 1;
                };
            };
        };
        return count;
    };
  )");
  // Counts upper triangular including diagonal: 5+4+3+2+1 = 15
  EXPECT_EQ(value, 15);
}

// ============================================================================
// Float Loops
// ============================================================================

TEST(LoopTest, for_float_accumulate) {
  auto value = executeString(R"(
    function main() f64 {
        var sum: f64 = 0.0;
        for (var i: i32 = 0; i < 5; i = i + 1) {
            sum = sum + 1.5;
        };
        return sum;
    };
  )");
  EXPECT_TRUE(std::holds_alternative<double>(value));
  EXPECT_NEAR(std::get<double>(value), 7.5, 0.001);
}

TEST(LoopTest, while_float_convergence) {
  auto value = executeString(R"(
    function main() i32 {
        var x: f64 = 1.0;
        var count: i32 = 0;
        while (x > 0.01) {
            x = x / 2.0;
            count = count + 1;
        };
        return count;
    };
  )");
  // 1.0 -> 0.5 -> 0.25 -> 0.125 -> 0.0625 -> 0.03125 -> 0.015625 -> 0.0078125
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(LoopTest, while_large_numbers) {
  // Tests while loop with larger computation
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        var i: i32 = 0;
        while (i < 1000) {
            sum = sum + 1;
            i = i + 1;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 1000);
}

TEST(LoopTest, for_zero_step_avoided) {
  // Ensure the loop terminates properly with step of 1
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            count = count + 1;
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 3);
}

TEST(LoopTest, while_complex_condition) {
  auto value = executeString(R"(
    function main() i32 {
        var a: i32 = 0;
        var b: i32 = 10;
        while (a < 5) {
            a = a + 1;
            b = b - 1;
        };
        return a + b;
    };
  )");
  EXPECT_EQ(value, 10);  // a=5, b=5, sum=10
}

// ============================================================================
// Break Statement
// ============================================================================

TEST(LoopTest, while_break_early) {
  auto value = executeString(R"(
    function main() i32 {
        var i: i32 = 0;
        while (i < 100) {
            if (i == 5) {
                break;
            };
            i = i + 1;
        };
        return i;
    };
  )");
  EXPECT_EQ(value, 5);  // Loop exits when i reaches 5
}

TEST(LoopTest, for_break_early) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 0; i < 100; i = i + 1) {
            if (i >= 5) {
                break;
            };
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 10);  // 0+1+2+3+4 = 10
}

TEST(LoopTest, break_find_first) {
  auto value = executeString(R"(
    function main() i32 {
        var found: i32 = -1;
        for (var i: i32 = 1; i <= 20; i = i + 1) {
            if (i * i > 50) {
                found = i;
                break;
            };
        };
        return found;
    };
  )");
  EXPECT_EQ(value, 8);  // 8*8 = 64 > 50
}

TEST(LoopTest, nested_break_inner) {
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        for (var i: i32 = 0; i < 5; i = i + 1) {
            for (var j: i32 = 0; j < 10; j = j + 1) {
                if (j >= 3) {
                    break;
                };
                count = count + 1;
            };
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 15);  // 5 outer iterations * 3 inner iterations (j=0,1,2)
}

// ============================================================================
// Continue Statement
// ============================================================================

TEST(LoopTest, while_continue_skip) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        var i: i32 = 0;
        while (i < 10) {
            i = i + 1;
            if (i / 2 * 2 == i) {
                continue;
            };
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 25);  // 1+3+5+7+9 = 25 (odd numbers)
}

TEST(LoopTest, for_continue_skip) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 0; i < 10; i = i + 1) {
            if (i / 2 * 2 == i) {
                continue;
            };
            sum = sum + i;
        };
        return sum;
    };
  )");
  EXPECT_EQ(value, 25);  // 1+3+5+7+9 = 25 (odd numbers only)
}

TEST(LoopTest, continue_skip_multiples) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 1; i <= 20; i = i + 1) {
            if (i / 3 * 3 == i) {
                continue;
            };
            sum = sum + 1;
        };
        return sum;
    };
  )");
  // Numbers 1-20 excluding multiples of 3 (3,6,9,12,15,18)
  // 20 - 6 = 14 numbers counted
  EXPECT_EQ(value, 14);
}

TEST(LoopTest, nested_continue) {
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            for (var j: i32 = 0; j < 5; j = j + 1) {
                if (j == 2) {
                    continue;
                };
                count = count + 1;
            };
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 12);  // 3 outer * (5 - 1 skipped) = 3 * 4 = 12
}

// ============================================================================
// Break and Continue Together
// ============================================================================

TEST(LoopTest, break_and_continue) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        for (var i: i32 = 0; i < 20; i = i + 1) {
            if (i / 2 * 2 == i) {
                continue;
            };
            if (i > 10) {
                break;
            };
            sum = sum + i;
        };
        return sum;
    };
  )");
  // Odd numbers less than or equal to 10: 1+3+5+7+9 = 25
  EXPECT_EQ(value, 25);
}

TEST(LoopTest, nested_break_continue) {
  auto value = executeString(R"(
    function main() i32 {
        var total: i32 = 0;
        for (var i: i32 = 0; i < 5; i = i + 1) {
            var innerSum: i32 = 0;
            for (var j: i32 = 0; j < 10; j = j + 1) {
                if (j / 2 * 2 == j) {
                    continue;
                };
                if (j > 5) {
                    break;
                };
                innerSum = innerSum + j;
            };
            total = total + innerSum;
        };
        return total;
    };
  )");
  // Inner loop adds odd numbers <= 5: 1+3+5 = 9
  // Outer loop runs 5 times: 5 * 9 = 45
  EXPECT_EQ(value, 45);
}

TEST(LoopTest, while_break_continue) {
  auto value = executeString(R"(
    function main() i32 {
        var sum: i32 = 0;
        var i: i32 = 0;
        while (true) {
            i = i + 1;
            if (i > 20) {
                break;
            };
            if (i / 3 * 3 == i) {
                continue;
            };
            sum = sum + i;
        };
        return sum;
    };
  )");
  // Sum of 1-20 excluding multiples of 3
  // 1+2+4+5+7+8+10+11+13+14+16+17+19+20 = 147
  EXPECT_EQ(value, 147);
}

// ============================================================================
// For-In Loops
// ============================================================================

TEST(ForInLoopTest, basic_iterator_sum) {
  // Test basic for-in loop with IIterator<T, Self> where the iterator is its
  // own "container"
  auto value = executeString(R"(
    class RangeIterator implements IIterator<i32, RangeIterator> {
      var current: i32;
      var end: i32;
      
      function init(start: i32, stop: i32) {
        this.current = start;
        this.end = stop;
      }
      
      function hasNext(self: ref RangeIterator) bool {
        return this.current < this.end;
      }
      
      function next(self: ref RangeIterator) i32 {
        var result = this.current;
        this.current = this.current + 1;
        return result;
      }
    }

    function main() i32 {
        var iter = RangeIterator(0, 5);
        var sum: i32 = 0;
        for (var x: i32 in iter) {
            sum = sum + x;
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 10);  // 0+1+2+3+4 = 10
}

TEST(ForInLoopTest, empty_iterator) {
  auto value = executeString(R"(
    class RangeIterator implements IIterator<i32, RangeIterator> {
      var current: i32;
      var end: i32;
      
      function init(start: i32, stop: i32) {
        this.current = start;
        this.end = stop;
      }
      
      function hasNext(self: ref RangeIterator) bool {
        return this.current < this.end;
      }
      
      function next(self: ref RangeIterator) i32 {
        var result = this.current;
        this.current = this.current + 1;
        return result;
      }
    }

    function main() i32 {
        var iter = RangeIterator(5, 5);  // Empty range
        var sum: i32 = 0;
        for (var x: i32 in iter) {
            sum = sum + x;
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 0);  // No iterations
}

TEST(ForInLoopTest, single_element) {
  auto value = executeString(R"(
    class SingleIterator implements IIterator<i32, SingleIterator> {
      var value: i32;
      var done: bool;
      
      function init(v: i32) {
        this.value = v;
        this.done = false;
      }
      
      function hasNext(self: ref SingleIterator) bool {
        return this.done == false;
      }
      
      function next(self: ref SingleIterator) i32 {
        this.done = true;
        return this.value;
      }
    }

    function main() i32 {
        var iter = SingleIterator(42);
        var result: i32 = 0;
        for (var x: i32 in iter) {
            result = x;
        }
        return result;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ForInLoopTest, variable_named_in) {
  // "in" should NOT be a reserved keyword - can use as variable name
  auto value = executeString(R"(
    function main() i32 {
        var in: i32 = 5;
        return in;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(ForInLoopTest, nested_forin) {
  auto value = executeString(R"(
    class RangeIterator implements IIterator<i32, RangeIterator> {
      var current: i32;
      var end: i32;
      
      function init(start: i32, stop: i32) {
        this.current = start;
        this.end = stop;
      }
      
      function hasNext(self: ref RangeIterator) bool {
        return this.current < this.end;
      }
      
      function next(self: ref RangeIterator) i32 {
        var result = this.current;
        this.current = this.current + 1;
        return result;
      }
    }

    function main() i32 {
        var sum: i32 = 0;
        var outer = RangeIterator(1, 4);
        for (var i: i32 in outer) {
            var inner = RangeIterator(1, 4);
            for (var j: i32 in inner) {
                sum = sum + i * j;
            }
        }
        return sum;
    }
  )");
  // (1*1 + 1*2 + 1*3) + (2*1 + 2*2 + 2*3) + (3*1 + 3*2 + 3*3)
  // = 6 + 12 + 18 = 36
  EXPECT_EQ(value, 36);
}

TEST(ForInLoopTest, break_in_forin) {
  auto value = executeString(R"(
    class RangeIterator implements IIterator<i32, RangeIterator> {
      var current: i32;
      var end: i32;
      
      function init(start: i32, stop: i32) {
        this.current = start;
        this.end = stop;
      }
      
      function hasNext(self: ref RangeIterator) bool {
        return this.current < this.end;
      }
      
      function next(self: ref RangeIterator) i32 {
        var result = this.current;
        this.current = this.current + 1;
        return result;
      }
    }

    function main() i32 {
        var iter = RangeIterator(0, 100);
        var sum: i32 = 0;
        for (var x: i32 in iter) {
            if (x == 5) { break; }
            sum = sum + x;
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 10);  // 0+1+2+3+4 = 10
}

TEST(ForInLoopTest, continue_in_forin) {
  auto value = executeString(R"(
    class RangeIterator implements IIterator<i32, RangeIterator> {
      var current: i32;
      var end: i32;
      
      function init(start: i32, stop: i32) {
        this.current = start;
        this.end = stop;
      }
      
      function hasNext(self: ref RangeIterator) bool {
        return this.current < this.end;
      }
      
      function next(self: ref RangeIterator) i32 {
        var result = this.current;
        this.current = this.current + 1;
        return result;
      }
    }

    function main() i32 {
        var iter = RangeIterator(1, 6);
        var sum: i32 = 0;
        for (var x: i32 in iter) {
            if (x == 3) { continue; }
            sum = sum + x;
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 12);  // 1+2+4+5 = 12 (skip 3)
}

// ============================================================================
// For-In Loop Error Cases
// ============================================================================

TEST(ForInLoopTest, error_class_without_iterator_interface) {
  // Class that has hasNext/next methods but doesn't implement IIterator
  EXPECT_ANY_THROW({
    executeString(R"(
      class NotAnIterator {
        var current: i32;
        var end: i32;
        
        function init(start: i32, stop: i32) {
          this.current = start;
          this.end = stop;
        }
        
        function hasNext() bool {
          return this.current < this.end;
        }
        
        function next() i32 {
          var result = this.current;
          this.current = this.current + 1;
          return result;
        }
      }

      function main() i32 {
          var iter = NotAnIterator(0, 5);
          var sum: i32 = 0;
          for (var x: i32 in iter) {
              sum = sum + x;
          }
          return sum;
      }
    )");
  });
}

TEST(ForInLoopTest, error_class_without_iterable_interface) {
  // Class that has iter() method but doesn't implement IIterable
  EXPECT_ANY_THROW({
    executeString(R"(
      class FakeIterator implements IIterator<i32, FakeIterator> {
        var done: bool;
        function init() { this.done = false; }
        function hasNext(self: ref FakeIterator) bool { return this.done == false; }
        function next(self: ref FakeIterator) i32 { this.done = true; return 42; }
      }

      class NotIterable {
        function init() {}
        function iter() FakeIterator {
          return FakeIterator();
        }
      }

      function main() i32 {
          var container = NotIterable();
          var sum: i32 = 0;
          for (var x: i32 in container) {
              sum = sum + x;
          }
          return sum;
      }
    )");
  });
}
