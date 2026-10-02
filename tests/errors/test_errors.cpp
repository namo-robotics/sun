// tests/errors/test_errors.cpp - Tests for error handling (try/catch/throw)

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

using sun::driver::executeString;
using sun::driver::executeStringWithStdlib;

// ============================================================================
// Basic try/catch Tests
// ============================================================================

TEST(Errors, basic_function_call) {
  // Functions that don't throw can be called directly, no try/catch needed
  auto value = executeString(R"(
    function get_value() i32 {
      return 42;
    }

    function main() i32 {
      return get_value();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Errors, throw_basic) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x * 2);
}

function main() i32 {
  return match mayThrow(5) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 10);
}

TEST(Errors, throw_triggers_catch) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x * 2);
}

function main() i32 {
  return match mayThrow(-5) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return 99;
    }
  };
}
)");
  EXPECT_EQ(value, 99);
}

TEST(Errors, try_catch_success_path) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function compute(a: i32, b: i32) Outcome<i32> {
  if (b == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(a / b);
}

function main() i32 {
  return match compute(20, 4) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 5);
}

// With native exceptions the thrown object is carried through the unwind and
// bound to the catch variable, so `e.code()` / `e.message()` are usable in the
// catch body (impossible under the old return-value error-union model).
TEST(Errors, catch_binding_code_is_usable) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), MyError(MyError) }

class MyError implements IError {
  init() {}
  const method code() i32 {
    return 7;
  }
  const method message() static_ptr<u8> {
    return "boom";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.MyError(MyError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  return match mayThrow(-1) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return e.code() + 100;
    }
  };
}
)");
  EXPECT_EQ(value, 107);
}

TEST(Errors, catch_binding_dispatches_to_concrete_type) {
  // The vtable carried in the exception reflects the concrete thrown class, so
  // dynamic dispatch picks the right override.
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), ErrA(ErrA), ErrB(ErrB) }

class ErrA implements IError {
  init() {}
  const method code() i32 {
    return 10;
  }
  const method message() static_ptr<u8> {
    return "a";
  }
}
class ErrB implements IError {
  init() {}
  const method code() i32 {
    return 20;
  }
  const method message() static_ptr<u8> {
    return "b";
  }
}

function pick(x: i32) Outcome<i32> {
  if (x == 1) {
    return Outcome.ErrA(ErrA());
  }
  return Outcome.ErrB(ErrB());
}

function main() i32 {
  return match pick(2) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return e.code();
    }
  };
}
)");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Typed catch clauses: multiple handlers matched by concrete error type
// ============================================================================

/** Keeps test fixtures and helpers local to this source file. */
namespace {
constexpr const char* kTypedErrors = R"(
    class ErrA implements IError {
      init() {}
      const method code() i32 { return 1; }
      const method message() static_ptr<u8> { return "a"; }
    }
    class ErrB implements IError {
      init() {}
      const method code() i32 { return 2; }
      const method message() static_ptr<u8> { return "b"; }
    }
    class ErrC implements IError {
      init() {}
      const method code() i32 { return 3; }
      const method message() static_ptr<u8> { return "c"; }
    }
)";
}  // namespace

TEST(Errors, typed_catch_selects_matching_clause) {
  auto value = executeString(std::string(kTypedErrors) + R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), ErrA(ErrA), ErrB(ErrB) }

function pick(x: i32) Outcome<i32> {
  if (x == 1) {
    return Outcome.ErrA(ErrA());
  }
  return Outcome.ErrB(ErrB());
}
function main() i32 {
  return match pick(2) {
    Outcome.Ok(value) => value,
    (e: ref ErrA) => {
      return 10;
    },
    (e: ref ErrB) => {
      return 20;
    },
    (e: ref IError) => {
      return 30;
    }
  };
}
)");
  EXPECT_EQ(value, 20);
}

TEST(Errors, typed_catch_falls_through_to_ierror) {
  auto value = executeString(std::string(kTypedErrors) + R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), ErrA(ErrA), ErrB(ErrB), ErrC(ErrC) }

function pick() Outcome<i32> {
  return Outcome.ErrC(ErrC());
}
function main() i32 {
  return match pick() {
    Outcome.Ok(value) => value,
    (e: ref ErrA) => {
      return 10;
    },
    (e: ref ErrB) => {
      return 20;
    },
    (e: ref IError) => {
      return e.code();
    }
  };
}
)");
  EXPECT_EQ(value, 3);  // ErrC.code()
}

TEST(Errors, typed_catch_concrete_binding_reads_field) {
  // The concrete binding is the real object, so a method unique to that class
  // (not on IError) is callable.
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), BoundsErr(BoundsErr) }

class BoundsErr implements IError {
  var idx_: i64;
  init(i: i64) {
    this.idx_ = i;
  }
  const method code() i32 {
    return 3;
  }
  const method message() static_ptr<u8> {
    return "oob";
  }
  method idx() i64 {
    return this.idx_;
  }
}
function may(x: i64) Outcome<i64> {
  if (x < 0) {
    return Outcome.BoundsErr(BoundsErr(77));
  }
  return Outcome.Ok(x);
}
function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<i32> {
    var r = try may(-1);
    return Outcome.Ok(0);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref BoundsErr) => {
      return e.idx();
    }
  };
}
)");
  EXPECT_EQ(value, 77);
}

TEST(Errors, typed_catch_unmatched_rethrows_to_outer) {
  // Inner try catches only ErrA; a thrown ErrB has no match and rethrows to the
  // enclosing try, which catches it.
  auto value = executeString(std::string(kTypedErrors) + R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), ErrA(ErrA), ErrB(ErrB) }

function pick(x: i32) Outcome<i32> {
  if (x == 1) {
    return Outcome.ErrA(ErrA());
  }
  return Outcome.ErrB(ErrB());
}
function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<i32> {
    return match pick(2) {
      Outcome.Ok(value) => Outcome.Ok(value),
      (e: ref ErrA) => {
        return Outcome.Ok(10);
      },
      Outcome.ErrB(error) => {
        return Outcome.ErrB(error);
      }
    };
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref ErrB) => {
      return 20;
    },
    Outcome.ErrA(error) => {
      return error.code();
    }
  };
}
)");
  EXPECT_EQ(value, 20);
}

TEST(Errors, try_catch_error_path) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function compute(a: i32, b: i32) Outcome<i32> {
  if (b == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(a / b);
}

function main() i32 {
  return match compute(20, 0) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, -1);
}

// ============================================================================
// Nested try/catch Tests
// ============================================================================

TEST(Errors, nested_try_catch) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function inner(x: i32) Outcome<i32> {
  if (x == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function outer(x: i32) Outcome<i32> {
  var result = try inner(x);
  return Outcome.Ok(result * 2);
}

function main() i32 {
  return match outer(5) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 10);
}

TEST(Errors, nested_error_propagation) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function inner(x: i32) Outcome<i32> {
  if (x == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function outer(x: i32) Outcome<i32> {
  var result = try inner(x);
  return Outcome.Ok(result * 2);
}

function main() i32 {
  return match outer(0) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, -1);
}

TEST(Errors, pass_mayThrow_to_function) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function foo(y: i32) i32 {
  return y;
}

function main() i32 {
  /** Propagates an error before evaluating the receiving call. */
  var invoke = () => Outcome<i32> { return Outcome.Ok(foo(try mayThrow(-5))); };
  return match invoke() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, -1);
}

TEST(Errors, pass_mayThrow_success) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function foo(y: i32) i32 {
  return y;
}

function main() i32 {
  /** Propagates failure before passing the value to another function. */
  var invoke = () => Outcome<i32> { return Outcome.Ok(foo(try mayThrow(1))); };
  return match invoke() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Division with Error Handling Tests
// ============================================================================

TEST(Errors, safe_divide_success) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function safeDivide(a: i32, b: i32) Outcome<i32> {
  if (b == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(a / b);
}

function main() i32 {
  return match safeDivide(42, 7) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 6);
}

TEST(Errors, safe_divide_by_zero) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function safeDivide(a: i32, b: i32) Outcome<i32> {
  if (b == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(a / b);
}

function main() i32 {
  return match safeDivide(42, 0) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, -1);
}

TEST(Errors, auto_safe_division_success) {
  // Valid division also works in functions declared to throw.
  auto value = executeString(R"(
/** Represents a fixture failure without a standard-library dependency. */
class Error implements IError {
  /** Creates the fixture failure. */
  init() {}
  /** Returns its numeric code. */
  const method code() i32 {
    return 1;
  }
  /** Returns a static description. */
  const method message() static_ptr<u8> {
    return "failure";
  }
}
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), Error(Error) }

function divide(a: i32, b: i32) Outcome<i32> {
  return Outcome.Ok(a / b);
}

function main() i32 {
  return match divide(100, 5) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Complex Expression Tests
// ============================================================================

TEST(Errors, try_catch_with_computation) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function compute(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x * 2);
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<i32> {
    var result = try compute(5);
    return Outcome.Ok(result + 1);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return 0;
    }
  };
}
)");
  EXPECT_EQ(value, 11);
}

TEST(Errors, try_catch_with_multiple_calls) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function add(a: i32, b: i32) Outcome<i32> {
  if (a < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(a + b);
}

function mul(a: i32, b: i32) Outcome<i32> {
  if (b < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(a * b);
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<i32> {
    var x = try mul(2, 3);
    return Outcome.Ok(try add(x, 4));
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 10);
}

TEST(Errors, try_catch_with_variable_args) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function combine(a: i32, b: i32, c: i32) Outcome<i32> {
  if (a < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(a + b + c);
}

function main() i32 {
  var x: i32 = 1;
  var y: i32 = 2;
  var z: i32 = 3;
  return match combine(x, y, z) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 6);
}

// ============================================================================
// Return Value from try/catch Tests
// ============================================================================

TEST(Errors, catch_returns_different_value) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayFail(x: i32) Outcome<i32> {
  if (x == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x * 10);
}

function main() i32 {
  return match mayFail(0) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return 42;
    }
  };
}
)");
  EXPECT_EQ(value, 42);
}

TEST(Errors, success_returns_original_value) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayFail(x: i32) Outcome<i32> {
  if (x == 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x * 10);
}

function main() i32 {
  return match mayFail(5) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return 42;
    }
  };
}
)");
  EXPECT_EQ(value, 50);
}

// ============================================================================
// Multiple throw points Tests
// ============================================================================

TEST(Errors, multiple_throw_conditions) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function validate(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  if (x > 100) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  return match validate(50) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 50);
}

TEST(Errors, first_condition_throws) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function validate(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  if (x > 100) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  return match validate(-5) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, -1);
}

TEST(Errors, second_condition_throws) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function validate(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  if (x > 100) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  return match validate(150) {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -2;
    }
  };
}
)");
  EXPECT_EQ(value, -2);
}

// ============================================================================
// Exceptions inside Loops Tests
// ============================================================================

TEST(Errors, throw_inside_for_loop) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x == 5) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum]() => Outcome<i32> {
    for (var i: i32 = 0; i < 10; i = i + 1) {
      sum = sum + try mayThrow(i);
    }
    return Outcome.Ok(sum);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // Loop runs i=0,1,2,3,4 then throws at i=5
  EXPECT_EQ(value, -1);
}

TEST(Errors, throw_inside_while_loop) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x == 5) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  var i: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum, ref i]() => Outcome<i32> {
    while (i < 10) {
      sum = sum + try mayThrow(i);
      i = i + 1;
    }
    return Outcome.Ok(sum);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // Loop runs i=0,1,2,3,4 then throws at i=5
  EXPECT_EQ(value, -1);
}

TEST(Errors, for_loop_completes_without_throw) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum]() => Outcome<i32> {
    for (var i: i32 = 0; i < 5; i = i + 1) {
      sum = sum + try mayThrow(i);
    }
    return Outcome.Ok(sum);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // 0+1+2+3+4 = 10
  EXPECT_EQ(value, 10);
}

TEST(Errors, while_loop_completes_without_throw) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x < 0) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  var i: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum, ref i]() => Outcome<i32> {
    while (i < 5) {
      sum = sum + try mayThrow(i);
      i = i + 1;
    }
    return Outcome.Ok(sum);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // 0+1+2+3+4 = 10
  EXPECT_EQ(value, 10);
}

TEST(Errors, throw_inside_nested_for_loops) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32, y: i32) Outcome<i32> {
  if (x == 2) {
    if (y == 3) {
      return Outcome.TestError(TestError());
    }
  }
  return Outcome.Ok(x + y);
}

function main() i32 {
  var sum: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum]() => Outcome<i32> {
    for (var i: i32 = 0; i < 5; i = i + 1) {
      for (var j: i32 = 0; j < 5; j = j + 1) {
        sum = sum + try mayThrow(i, j);
      }
    }
    return Outcome.Ok(sum);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // Throws when i=2, j=3
  EXPECT_EQ(value, -1);
}

TEST(Errors, throw_inside_for_loop_with_break) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x == 8) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum]() => Outcome<i32> {
    for (var i: i32 = 0; i < 10; i = i + 1) {
      if (i == 5) {
        break;
      }
      sum = sum + try mayThrow(i);
    }
    return Outcome.Ok(sum);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // Loop breaks at i=5 before throw at i=8
  // 0+1+2+3+4 = 10
  EXPECT_EQ(value, 10);
}

TEST(Errors, throw_inside_while_loop_with_continue) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function mayThrow(x: i32) Outcome<i32> {
  if (x == 10) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  var i: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum, ref i]() => Outcome<i32> {
    while (i < 8) {
      i = i + 1;
      if (i / 2 * 2 == i) {
        continue;
      }
      sum = sum + try mayThrow(i);
    }
    return Outcome.Ok(sum);
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // Adds odd numbers 1+3+5+7 = 16
  EXPECT_EQ(value, 16);
}

TEST(Errors, throw_after_loop_iteration) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function process(x: i32) Outcome<i32> {
  if (x > 20) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum]() => Outcome<i32> {
    for (var i: i32 = 1; i <= 5; i = i + 1) {
      sum = sum + i;
    }
    return Outcome.Ok(try process(sum));
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  // sum = 1+2+3+4+5 = 15, process(15) succeeds
  EXPECT_EQ(value, 15);
}

TEST(Errors, throw_after_loop_exceeds_limit) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function process(x: i32) Outcome<i32> {
  if (x > 20) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(x);
}

function main() i32 {
  var sum: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref sum]() => Outcome<i32> {
    for (var i: i32 = 1; i <= 10; i = i + 1) {
      sum = sum + i;
    }
    return Outcome.Ok(try process(sum));
  };
  return match attempt_0() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
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
using std;
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), Error(Error) }

function boom(path: ref String) Outcome<void> {
  return Outcome.Error(Error(-7, path));

  return Outcome.Ok;
}

function main() i32 {
  var a = make_heap_allocator();
  var path = String(a, "/tmp/");
  path.append("computed.txt");
  /** Executes a fallible operation. */
  var attempt_0 = [ref path]() => Outcome<void> {
    try boom(path);

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      // The message outlives the String's scope: Error keeps its own copy.
      var msg: String = e.message();
      if (not msg.equals(path)) {
        return -2;
      }
      return e.code();
    }
  };
  return 0;
}
)");
  EXPECT_EQ(value, -7);
}

TEST(Errors, computed_error_message_survives_the_string_it_came_from) {
  auto value = executeStringWithStdlib(R"(
using std;
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), Error(Error) }

// The String is built, moved into the Error and dropped here; only the
// Error's copy is left for the caller to read.
function make(a: ref HeapAllocator) Outcome<void> {
  var msg = String(a, "gone");
  msg.append(" by now");
  return Outcome.Error(Error(3, msg));

  return Outcome.Ok;
}

function main() i32 {
  var a = make_heap_allocator();
  /** Executes a fallible operation. */
  var attempt_0 = [ref a]() => Outcome<void> {
    try make(a);

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      var text: String = e.message();
      if (text.length() != 11) {
        return -2;
      }
      // 'g' is 103: the clone is real bytes, not freed storage.
      if (unsafe { text.unsafe_at(0); } != 103) {
        return -3;
      }
      return e.code();
    }
  };
  return 0;
}
)");
  EXPECT_EQ(value, 3);
}

TEST(Errors, error_still_takes_a_literal_message) {
  auto value = executeStringWithStdlib(R"(
    using std;

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
    using std;

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
  // The builtin message contract is independent of the standard library.
  auto value = executeString(R"(
    class Boom implements IError {
      init() {}
      const method code() i32 { return 9; }
      const method message() static_ptr<u8> { return "boom"; }
    }

    function main() i32 {
      var b: Boom = Boom();
      return b.code() + _convert<i32>(b.message().length());
    }
  )");
  EXPECT_EQ(value, 13);  // 9 + 4
}

TEST(Errors, throw_and_catch_work_without_stdlib) {
  // IError is a builtin and throw/catch lower to native exceptions, so a
  // program can throw, catch through the IError binding and read the
  // literal message with nothing imported.
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), Boom(Boom) }

class Boom implements IError {
  init() {}
  const method code() i32 {
    return 9;
  }
  const method message() static_ptr<u8> {
    return "boom";
  }
}

function fail() Outcome<i32> {
  return Outcome.Boom(Boom());
}

function main() i32 {
  return match fail() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return e.code() + _convert<i32>(e.message().length());
    }
  };
}
)");
  EXPECT_EQ(value, 13);  // 9 + 4
}

// ============================================================================
// Call diagnostics name the callee (issue #84)
// ============================================================================

TEST(Errors, argument_mismatch_on_a_method_names_the_method) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        class Counter {
          var n: i32;
          init() { this.n = 0; }
          method add(step: i32) void { this.n = this.n + step; }
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
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
        class Pair {
          var a: i32;
          init(a: i32) { this.a = a; }
          init(a: i32, b: i32) { this.a = a + b; }
        }

        function main() i32 {
          var p: Pair = Pair(true);
          return 0;
        }
      )"),
                                "candidate: init(i32, i32)");
}

// ============================================================================
// Try-catch is a statement, not a value
// ============================================================================

// Only a match arm's body and an unsafe block's body evaluate to their last
// statement; a `try` body does not, so a try-catch cannot be bound.
TEST(Errors, binding_a_try_catch_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function g() i32 throws IError { return 5; }
    function f() i32 {
        var x = try { g(); } catch (e: ref IError) { 0; };
        return 1;
    }
    function main() i32 { return f(); }
  )"),
               sun::support::SunError);
}

// The supported shape: return from inside the try.
TEST(Errors, returning_from_inside_the_try_is_the_value_form) {
  auto value = executeString(R"(
/** Represents a fixture failure without a standard-library dependency. */
class Error implements IError {
  /** Creates the fixture failure. */
  init() {}
  /** Returns its numeric code. */
  const method code() i32 {
    return 1;
  }
  /** Returns a static description. */
  const method message() static_ptr<u8> {
    return "failure";
  }
}
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), Error(Error) }

function g() Outcome<i32> {
  return Outcome.Ok(5);
}
function main() i32 {
  return match g() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -1;
    }
  };
}
)");
  EXPECT_EQ(value, 5);
}

TEST(Errors, bool_leading_class_return_field_access) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}
class R {
  public var ok: bool;
  public var t: i64;
  init() {
    this.ok = true;
    this.t = 37;
  }
}
function make(fail: bool) Outcome<R> {
  if (fail) {
    return Outcome.TestError(TestError());
  }
  return Outcome.Ok(R());
}
function main() i32 {
  var result: i32 = 0;
  /** Executes a fallible operation. */
  var attempt_0 = [ref result]() => Outcome<i32> {
    if ((try make(false)).ok) {
      result = 40;
    }
    if ((try make(false)).t != 37) {
      return Outcome.Ok(1);
    }
    if ((try make(true)).ok) {
      return Outcome.Ok(2);
    }
    return Outcome.Ok(3);
  };
  match attempt_0() {
    Outcome.Ok(value) => {
      return value;
    },
    (e: ref IError) => {
      result = result + 2;
    }
  };
  return result;
}
)");
  EXPECT_EQ(value, 42);
}
