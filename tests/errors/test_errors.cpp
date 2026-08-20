// tests/errors/test_errors.cpp - Tests for error handling (try/catch/throw)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic try/catch Tests
// ============================================================================

TEST(Errors, basic_function_call) {
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

TEST(Errors, throw_basic) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
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

TEST(Errors, throw_triggers_catch) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
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

TEST(Errors, try_catch_success_path) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function compute(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw TestError();
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

// With native exceptions the thrown object is carried through the unwind and
// bound to the catch variable, so `e.code()` / `e.message()` are usable in the
// catch body (impossible under the old return-value error-union model).
TEST(Errors, catch_binding_code_is_usable) {
  auto value = executeString(R"(
    class MyError implements IError {
      function init() {}
      function code() i32 { return 7; }
      function message() static_ptr<u8> { return "boom"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw MyError();
      }
      return x;
    }

    function main() i32 {
      try {
        return mayThrow(-1);
      } catch (e: IError) {
        return e.code() + 100;
      }
    }
  )");
  EXPECT_EQ(value, 107);
}

TEST(Errors, catch_binding_dispatches_to_concrete_type) {
  // The vtable carried in the exception reflects the concrete thrown class, so
  // dynamic dispatch picks the right override.
  auto value = executeString(R"(
    class ErrA implements IError {
      function init() {}
      function code() i32 { return 10; }
      function message() static_ptr<u8> { return "a"; }
    }
    class ErrB implements IError {
      function init() {}
      function code() i32 { return 20; }
      function message() static_ptr<u8> { return "b"; }
    }

    function pick(x: i32) i32, IError {
      if (x == 1) { throw ErrA(); }
      throw ErrB();
    }

    function main() i32 {
      try {
        return pick(2);
      } catch (e: IError) {
        return e.code();
      }
    }
  )");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Typed catch clauses: multiple handlers matched by concrete error type
// ============================================================================

namespace {
constexpr const char* kTypedErrors = R"(
    class ErrA implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "a"; }
    }
    class ErrB implements IError {
      function init() {}
      function code() i32 { return 2; }
      function message() static_ptr<u8> { return "b"; }
    }
    class ErrC implements IError {
      function init() {}
      function code() i32 { return 3; }
      function message() static_ptr<u8> { return "c"; }
    }
)";
}  // namespace

TEST(Errors, typed_catch_selects_matching_clause) {
  auto value = executeString(std::string(kTypedErrors) + R"(
    function pick(x: i32) i32, IError {
      if (x == 1) { throw ErrA(); }
      throw ErrB();
    }
    function main() i32 {
      try {
        return pick(2);
      } catch (e: ErrA) {
        return 10;
      } catch (e: ErrB) {
        return 20;
      } catch (e: IError) {
        return 30;
      }
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(Errors, typed_catch_falls_through_to_ierror) {
  auto value = executeString(std::string(kTypedErrors) + R"(
    function pick() i32, IError { throw ErrC(); }
    function main() i32 {
      try {
        return pick();
      } catch (e: ErrA) {
        return 10;
      } catch (e: ErrB) {
        return 20;
      } catch (e: IError) {
        return e.code();
      }
    }
  )");
  EXPECT_EQ(value, 3);  // ErrC.code()
}

TEST(Errors, typed_catch_concrete_binding_reads_field) {
  // The concrete binding is the real object, so a method unique to that class
  // (not on IError) is callable.
  auto value = executeString(R"(
    class BoundsErr implements IError {
      var idx_: i64;
      function init(i: i64) { this.idx_ = i; }
      function code() i32 { return 3; }
      function message() static_ptr<u8> { return "oob"; }
      function idx() i64 { return this.idx_; }
    }
    function may(x: i64) i64, IError {
      if (x < 0) { throw BoundsErr(77); }
      return x;
    }
    function main() i32 {
      try {
        var r = may(-1);
        return 0;
      } catch (e: BoundsErr) {
        return e.idx();
      }
    }
  )");
  EXPECT_EQ(value, 77);
}

TEST(Errors, typed_catch_unmatched_rethrows_to_outer) {
  // Inner try catches only ErrA; a thrown ErrB has no match and rethrows to the
  // enclosing try, which catches it.
  auto value = executeString(std::string(kTypedErrors) + R"(
    function pick(x: i32) i32, IError {
      if (x == 1) { throw ErrA(); }
      throw ErrB();
    }
    function main() i32 {
      try {
        try {
          return pick(2);
        } catch (e: ErrA) {
          return 10;
        }
      } catch (e: ErrB) {
        return 20;
      }
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(Errors, try_catch_error_path) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function compute(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw TestError();
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

TEST(Errors, nested_try_catch) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function inner(x: i32) i32, IError {
      if (x == 0) {
        throw TestError();
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

TEST(Errors, nested_error_propagation) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function inner(x: i32) i32, IError {
      if (x == 0) {
        throw TestError();
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

TEST(Errors, pass_mayThrow_to_function) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
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

TEST(Errors, pass_mayThrow_success) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
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

TEST(Errors, safe_divide_success) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function safeDivide(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw TestError();
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

TEST(Errors, safe_divide_by_zero) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function safeDivide(a: i32, b: i32) i32, IError {
      if (b == 0) {
        throw TestError();
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

TEST(Errors, auto_safe_division_success) {
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

TEST(Errors, auto_safe_division_by_zero) {
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

TEST(Errors, try_catch_with_computation) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function compute(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
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

TEST(Errors, try_catch_with_multiple_calls) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function add(a: i32, b: i32) i32, IError {
      if (a < 0) {
        throw TestError();
      }
      return a + b;
    }

    function mul(a: i32, b: i32) i32, IError {
      if (b < 0) {
        throw TestError();
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

TEST(Errors, try_catch_with_variable_args) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function combine(a: i32, b: i32, c: i32) i32, IError {
      if (a < 0) {
        throw TestError();
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

TEST(Errors, catch_returns_different_value) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayFail(x: i32) i32, IError {
      if (x == 0) {
        throw TestError();
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

TEST(Errors, success_returns_original_value) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayFail(x: i32) i32, IError {
      if (x == 0) {
        throw TestError();
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

TEST(Errors, multiple_throw_conditions) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function validate(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
      }
      if (x > 100) {
        throw TestError();
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

TEST(Errors, first_condition_throws) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function validate(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
      }
      if (x > 100) {
        throw TestError();
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

TEST(Errors, second_condition_throws) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function validate(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
      }
      if (x > 100) {
        throw TestError();
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

TEST(Errors, throw_inside_for_loop) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x == 5) {
        throw TestError();
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

TEST(Errors, throw_inside_while_loop) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x == 5) {
        throw TestError();
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

TEST(Errors, for_loop_completes_without_throw) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
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

TEST(Errors, while_loop_completes_without_throw) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x < 0) {
        throw TestError();
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

TEST(Errors, throw_inside_nested_for_loops) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32, y: i32) i32, IError {
      if (x == 2) {
        if (y == 3) {
          throw TestError();
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

TEST(Errors, throw_inside_for_loop_with_break) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x == 8) {
        throw TestError();
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

TEST(Errors, throw_inside_while_loop_with_continue) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function mayThrow(x: i32) i32, IError {
      if (x == 10) {
        throw TestError();
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

TEST(Errors, throw_after_loop_iteration) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function process(x: i32) i32, IError {
      if (x > 20) {
        throw TestError();
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

TEST(Errors, throw_after_loop_exceeds_limit) {
  auto value = executeString(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function process(x: i32) i32, IError {
      if (x > 20) {
        throw TestError();
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

// ============================================================================
// Error with a message computed at runtime (issue #84)
// ============================================================================

TEST(Errors, error_carries_a_computed_string_message) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function boom(path: ref String) void, IError {
      throw Error(-7, path);
    }

    function main() i32 {
      var a = make_heap_allocator();
      var path = String(a, "/tmp/");
      path.append("computed.txt");
      try {
        boom(path);
      } catch (e: IError) {
        // The message outlives the String's scope: Error keeps its own copy.
        var msg: String = e.message();
        if (not msg.equals(path)) { return -2; }
        return e.code();
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, -7);
}

TEST(Errors, computed_error_message_survives_the_string_it_came_from) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    // The String is built, moved into the Error and dropped here; only the
    // Error's copy is left for the caller to read.
    function make(a: ref HeapAllocator) void, IError {
      var msg = String(a, "gone");
      msg.append(" by now");
      throw Error(3, msg);
    }

    function main() i32 {
      var a = make_heap_allocator();
      try {
        make(a);
      } catch (e: IError) {
        var text: String = e.message();
        if (text.length() != 11) { return -2; }
        // 'g' is 103: the clone is real bytes, not freed storage.
        if (text.at(0) != 103) { return -3; }
        return e.code();
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Errors, error_still_takes_a_literal_message) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var e: Error = Error(5, "plain literal");
      var msg: String = e.message();
      return e.code() + _convert<i32>(msg.length());
    }
  )");
  EXPECT_EQ(value, 18);  // 5 + 13
}

TEST(Errors, message_returns_an_independent_clone_each_time) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var e: Error = Error(1, "abc");
      var first: String = e.message();
      first.append("!");
      // Mutating one clone must not leak into the next.
      var second: String = e.message();
      return _convert<i32>(first.length() * 10 + second.length());
    }
  )");
  EXPECT_EQ(value, 43);  // first grew to 4, second is a fresh 3
}

TEST(Errors, without_stdlib_message_stays_literal_only) {
  // No stdlib loaded: there is no String class, so IError keeps its
  // registered static_ptr<u8> message contract.
  auto value = executeString(R"(
    class Boom implements IError {
      function init() {}
      function code() i32 { return 9; }
      function message() static_ptr<u8> { return "boom"; }
    }

    function main() i32 {
      var b: Boom = Boom();
      return b.code() + _convert<i32>(_static_ptr_len<u8>(b.message()));
    }
  )");
  EXPECT_EQ(value, 13);  // 9 + 4
}

// ============================================================================
// Call diagnostics name the callee (issue #84)
// ============================================================================

TEST(Errors, argument_mismatch_on_a_method_names_the_method) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        class Counter {
          var n: i32;
          function init() { this.n = 0; }
          function add(step: i32) void { this.n = this.n + step; }
        }

        function main() i32 {
          var c: Counter = Counter();
          c.add(1.5);
          return 0;
        }
      )"),
      "call to 'add'");
}

TEST(Errors, no_matching_constructor_lists_the_candidates) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
        class Pair {
          var a: i32;
          function init(a: i32) { this.a = a; }
          function init(a: i32, b: i32) { this.a = a + b; }
        }

        function main() i32 {
          var p: Pair = Pair(true);
          return 0;
        }
      )"),
      "candidate: init(i32, i32)");
}
