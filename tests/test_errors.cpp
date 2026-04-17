// tests/test_errors.cpp - Tests for error handling (try/catch/throw)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic try/catch Tests
// ============================================================================

TEST(ErrorTest, basic_function_call) {
  // Functions that don't throw can be called directly, no try/catch needed
  auto value = executeString(R"(
    function getValue() i32 {
      return 42;
    }

    function main() i32 {
      return getValue();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ErrorTest, throw_basic) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      return x * 2;
    }

    function main() i32 {
      try {
        mayThrow(5);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(ErrorTest, throw_triggers_catch) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      return x * 2;
    }

    function main() i32 {
      try {
        mayThrow(-5);
      } catch (e: IError) {
        return 99;
      }
    }
  )");
  EXPECT_EQ(value, 99);
}

TEST(ErrorTest, try_catch_success_path) {
  auto value = executeString(R"(
    function compute(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw 1;
      }
      return a / b;
    }

    function main() i32 {
      try {
        compute(20, 4);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(ErrorTest, try_catch_error_path) {
  auto value = executeString(R"(
    function compute(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw 1;
      }
      return a / b;
    }

    function main() i32 {
      try {
        compute(20, 0);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, -1);
}

// ============================================================================
// Nested try/catch Tests
// ============================================================================

TEST(ErrorTest, nested_try_catch) {
  auto value = executeString(R"(
    function inner(x: i32) i32, IError {
      if (x == 0) {
        throw 1;
      }
      return x;
    }

    function outer(x: i32) i32, IError {
      var result = inner(x);
      return result * 2;
    }

    function main() i32 {
      try {
        outer(5);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(ErrorTest, nested_error_propagation) {
  auto value = executeString(R"(
    function inner(x: i32) i32, IError {
      if (x == 0) {
        throw 1;
      }
      return x;
    }

    function outer(x: i32) i32, IError {
      var result = inner(x);
      return result * 2;
    }

    function main() i32 {
      try {
        outer(0);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, -1);
}

TEST(ErrorTest, pass_mayThrow_to_function) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      return x;
    }

    function foo(y: i32) i32 {
      return y;
    }

    function main() i32 {
      try {
        foo(mayThrow(-5));
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, -1);
}

TEST(ErrorTest, pass_mayThrow_success) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      return x;
    }

    function foo(y: i32) i32 {
      return y;
    }

    function main() i32 {
      try {
        foo(mayThrow(1));
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Division with Error Handling Tests
// ============================================================================

TEST(ErrorTest, safe_divide_success) {
  auto value = executeString(R"(
    function safeDivide(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw 1;
      }
      return a / b;
    }

    function main() i32 {
      try {
        safeDivide(42, 7);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(ErrorTest, safe_divide_by_zero) {
  auto value = executeString(R"(
    function safeDivide(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw 1;
      }
      return a / b;
    }

    function main() i32 {
      try {
        safeDivide(42, 0);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, -1);
}

TEST(ErrorTest, auto_safe_division_success) {
  // Functions declared with IError automatically check for division by zero
  auto value = executeString(R"(
    function divide(a: i32, b: i32) i32, IError {
      return a / b;
    }

    function main() i32 {
      try {
        divide(100, 5);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(ErrorTest, auto_safe_division_by_zero) {
  // Functions declared with IError automatically check for division by zero
  auto value = executeString(R"(
    function divide(a: i32, b: i32) i32, IError {
      return a / b;
    }

    function main() i32 {
      try {
        divide(1, 0);
      } catch (e: IError) {
        return 0;
      }
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Complex Expression Tests
// ============================================================================

TEST(ErrorTest, try_catch_with_computation) {
  auto value = executeString(R"(
    function compute(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      return x * 2;
    }

    function main() i32 {
      try {
        var result = compute(5);
        result + 1;
      } catch (e: IError) {
        return 0;
      }
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(ErrorTest, try_catch_with_multiple_calls) {
  auto value = executeString(R"(
    function add(a: i32, b: i32) i32, IError {
      if (a < 0) {
        throw 1;
      }
      return a + b;
    }

    function mul(a: i32, b: i32) i32, IError {
      if (b < 0) {
        throw 1;
      }
      return a * b;
    }

    function main() i32 {
      try {
        var x = mul(2, 3);
        add(x, 4);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(ErrorTest, try_catch_with_variable_args) {
  auto value = executeString(R"(
    function combine(a: i32, b: i32, c: i32) i32, IError {
      if (a < 0) {
        throw 1;
      }
      return a + b + c;
    }

    function main() i32 {
      var x: i32 = 1;
      var y: i32 = 2;
      var z: i32 = 3;
      try {
        combine(x, y, z);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 6);
}

// ============================================================================
// Return Value from try/catch Tests
// ============================================================================

TEST(ErrorTest, catch_returns_different_value) {
  auto value = executeString(R"(
    function mayFail(x: i32) i32, IError {
      if (x == 0) {
        throw 1;
      }
      return x * 10;
    }

    function main() i32 {
      try {
        mayFail(0);
      } catch (e: IError) {
        return 42;
      }
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ErrorTest, success_returns_original_value) {
  auto value = executeString(R"(
    function mayFail(x: i32) i32, IError {
      if (x == 0) {
        throw 1;
      }
      return x * 10;
    }

    function main() i32 {
      try {
        mayFail(5);
      } catch (e: IError) {
        return 42;
      }
    }
  )");
  EXPECT_EQ(value, 50);
}

// ============================================================================
// Multiple throw points Tests
// ============================================================================

TEST(ErrorTest, multiple_throw_conditions) {
  auto value = executeString(R"(
    function validate(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      if (x > 100) {
        throw 2;
      }
      return x;
    }

    function main() i32 {
      try {
        validate(50);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 50);
}

TEST(ErrorTest, first_condition_throws) {
  auto value = executeString(R"(
    function validate(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      if (x > 100) {
        throw 2;
      }
      return x;
    }

    function main() i32 {
      try {
        validate(-5);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, -1);
}

TEST(ErrorTest, second_condition_throws) {
  auto value = executeString(R"(
    function validate(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      if (x > 100) {
        throw 2;
      }
      return x;
    }

    function main() i32 {
      try {
        validate(150);
      } catch (e: IError) {
        return -2;
      }
    }
  )");
  EXPECT_EQ(value, -2);
}

// ============================================================================
// Exceptions inside Loops Tests
// ============================================================================

TEST(ErrorTest, throw_inside_for_loop) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x == 5) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      try {
        for (var i: i32 = 0; i < 10; i = i + 1) {
          sum = sum + mayThrow(i);
        };
        return sum;
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // Loop runs i=0,1,2,3,4 then throws at i=5
  EXPECT_EQ(value, -1);
}

TEST(ErrorTest, throw_inside_while_loop) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x == 5) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      var i: i32 = 0;
      try {
        while (i < 10) {
          sum = sum + mayThrow(i);
          i = i + 1;
        };
        return sum;
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // Loop runs i=0,1,2,3,4 then throws at i=5
  EXPECT_EQ(value, -1);
}

TEST(ErrorTest, for_loop_completes_without_throw) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      try {
        for (var i: i32 = 0; i < 5; i = i + 1) {
          sum = sum + mayThrow(i);
        };
        return sum;
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // 0+1+2+3+4 = 10
  EXPECT_EQ(value, 10);
}

TEST(ErrorTest, while_loop_completes_without_throw) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      var i: i32 = 0;
      try {
        while (i < 5) {
          sum = sum + mayThrow(i);
          i = i + 1;
        };
        return sum;
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // 0+1+2+3+4 = 10
  EXPECT_EQ(value, 10);
}

TEST(ErrorTest, throw_inside_nested_for_loops) {
  auto value = executeString(R"(
    function mayThrow(x: i32, y: i32) i32, IError {
      if (x == 2) {
        if (y == 3) {
          throw 1;
        };
      };
      return x + y;
    }

    function main() i32 {
      var sum: i32 = 0;
      try {
        for (var i: i32 = 0; i < 5; i = i + 1) {
          for (var j: i32 = 0; j < 5; j = j + 1) {
            sum = sum + mayThrow(i, j);
          };
        };
        return sum;
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // Throws when i=2, j=3
  EXPECT_EQ(value, -1);
}

TEST(ErrorTest, throw_inside_for_loop_with_break) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x == 8) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      try {
        for (var i: i32 = 0; i < 10; i = i + 1) {
          if (i == 5) {
            break;
          };
          sum = sum + mayThrow(i);
        };
        return sum;
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // Loop breaks at i=5 before throw at i=8
  // 0+1+2+3+4 = 10
  EXPECT_EQ(value, 10);
}

TEST(ErrorTest, throw_inside_while_loop_with_continue) {
  auto value = executeString(R"(
    function mayThrow(x: i32) i32, IError {
      if (x == 10) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      var i: i32 = 0;
      try {
        while (i < 8) {
          i = i + 1;
          if (i / 2 * 2 == i) {
            continue;
          };
          sum = sum + mayThrow(i);
        };
        return sum;
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // Adds odd numbers 1+3+5+7 = 16
  EXPECT_EQ(value, 16);
}

TEST(ErrorTest, throw_after_loop_iteration) {
  auto value = executeString(R"(
    function process(x: i32) i32, IError {
      if (x > 20) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      try {
        for (var i: i32 = 1; i <= 5; i = i + 1) {
          sum = sum + i;
        };
        process(sum);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // sum = 1+2+3+4+5 = 15, process(15) succeeds
  EXPECT_EQ(value, 15);
}

TEST(ErrorTest, throw_after_loop_exceeds_limit) {
  auto value = executeString(R"(
    function process(x: i32) i32, IError {
      if (x > 20) {
        throw 1;
      }
      return x;
    }

    function main() i32 {
      var sum: i32 = 0;
      try {
        for (var i: i32 = 1; i <= 10; i = i + 1) {
          sum = sum + i;
        };
        process(sum);
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  // sum = 1+2+...+10 = 55 > 20, so process throws
  EXPECT_EQ(value, -1);
}
