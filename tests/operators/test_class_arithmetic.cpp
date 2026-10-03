/** Exercises class arithmetic through the ordinary method-call machinery. */
#include <gtest/gtest.h>
#include "driver/execution_utils.h"

using sun::driver::executeString;
using sun::support::SunError;

/** Checks operator overload selection and unchanged primitive precedence. */
TEST(Operators_ClassArithmetic, overloads_and_precedence) {
  EXPECT_EQ(executeString(R"(
    /** Stores a numeric operand for overloaded arithmetic. */
    class Number {
      var value: i32;
      /** Initializes the operand. */
      init(value: i32) { this.value = value; }
      /** Adds a borrowed object. */
      public const method __add__(other: const ref Number) i32 { return this.value + other.value; }
      /** Multiplies by a scalar. */
      public const method __multiply__(other: i32) i32 { return this.value * other; }
      /** Multiplies by a borrowed object. */
      public const method __multiply__(other: const ref Number) i32 { return this.value * other.value; }
    }
    /** Exercises each overload without moving either input. */
    function main() i32 {
      var a = Number(3);
      var b = Number(4);
      return (a + b) + (a * b) + (a * 5) + 2 * 4;
    }
  )"), 42);
}

/** Checks generic dispatch with owning results and temporary receivers. */
TEST(Operators_ClassArithmetic, generics_and_temporaries) {
  EXPECT_EQ(executeString(R"(
    /** Owns an arithmetic value. */
    class Box<T: _Numeric> {
      var value: T;
      /** Initializes the owned value. */
      init(value: T) { this.value = value; }
      /** Allocates an owning result while borrowing both operands. */
      public const method __add__(other: const ref Box<T>) Box<T> {
        return Box<T>(this.value + other.value);
      }
    }
    /** Verifies that chained temporary results remain valid. */
    function main() i32 {
      var result = Box<i32>(10) + Box<i32>(12) + Box<i32>(20);
      return result.value;
    }
  )"), 42);
}

/** Checks explicit result propagation from an arithmetic hook. */
TEST(Operators_ClassArithmetic, fallible_operator) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Supplies the error protocol for a fallible result. */
    class Failure implements IError {
      /** Returns an error code. */
      public const method code() i32 { return 1; }
      /** Returns an error message. */
      public const method message() String { return String("failed"); }
    }
    /** Wraps an integer result. */
    enum Outcome<T> { Ok(T), Error(Failure) }
    /** Provides a fallible operator. */
    class Number {
      /** Produces an explicit successful result. */
      public const method __multiply__(value: i32) Outcome<i32> { return Outcome.Ok(value); }
    }
    /** Propagates operator errors explicitly. */
    function calculate() Outcome<i32> { var n = Number(); return Outcome.Ok(try (n * 42)); }
    /** Handles the explicitly propagated result. */
    function main() i32 { return match calculate() { Outcome.Ok(value) => value, _ => 0 }; }
  )"), 42);
}

/** Checks ordinary visibility rules for arithmetic methods. */
TEST(Operators_ClassArithmetic, private_hook_is_rejected) {
  EXPECT_THROW(executeString(R"(
    /** Keeps private hooks inside their declaring module. */
    public module hidden {
    /** Hides its arithmetic method. */
    public class Number {
      /** Is intentionally inaccessible to callers. */
      const method __add__(value: i32) i32 { return value; }
    }
    }
    /** Attempts an inaccessible call through operator syntax. */
    function main() i32 { var n = hidden.Number(); return n + 42; }
  )"), SunError);
}

/** Checks const receiver restrictions on mutating hooks. */
TEST(Operators_ClassArithmetic, mutable_hook_requires_mutable_receiver) {
  EXPECT_THROW(executeString(R"(
    /** Supplies a mutating hook. */
    class Number {
      /** Requires mutable access. */
      public method __add__(value: i32) i32 { return value; }
    }
    /** Attempts to mutate a constant receiver. */
    function use(n: const ref Number) i32 { return n + 42; }
    /** Calls the failing helper. */
    function main() i32 { var n = Number(); return use(n); }
  )"), SunError);
}

/** Checks that each operand executes once, with the receiver first. */
TEST(Operators_ClassArithmetic, evaluation_order) {
  EXPECT_EQ(executeString(R"(
    var order: i32 = 0;
    /** Supplies a borrowed scalar operation. */
    class Number {
      /** Returns the accumulated evaluation trace. */
      public const method __add__(value: i32) i32 { return order; }
    }
    /** Records evaluation of the left operand. */
    function left() Number { order = order * 10 + 1; return Number(); }
    /** Records evaluation of the right operand. */
    function right() i32 { order = order * 10 + 2; return 0; }
    /** Evaluates the operands exactly once. */
    function main() i32 { return left() + right(); }
  )"), 12);
}

/** Checks that a reference returned by an operator remains a borrow. */
TEST(Operators_ClassArithmetic, reference_result) {
  EXPECT_EQ(executeString(R"(
    /** Owns the value returned by reference. */
    class Number {
      var value: i32 = 42;
      /** Borrows the stored value. */
      public method __add__(unused: i32) ref i32 { return this.value; }
    }
    /** Reads the operator's borrowed result. */
    function main() i32 { var n = Number(); var value: ref i32 = n + 0; return value; }
  )"), 42);
}

/** Checks that missing and unsupported operator hooks fail normally. */
TEST(Operators_ClassArithmetic, missing_and_unsupported_hooks) {
  EXPECT_THROW(executeString(R"(
    /** Has no arithmetic hooks. */
    class Number { var value: i32 = 0; }
    /** Attempts an unsupported addition. */
    function main() i32 { var n = Number(); return n + 1; }
  )"), SunError);
  EXPECT_THROW(executeString(R"(
    /** Has no subtraction support. */
    class Number { var value: i32 = 0; }
    /** Attempts an unsupported subtraction. */
    function main() i32 { var n = Number(); return n - 1; }
  )"), SunError);
}

/** Checks that value parameters move rather than implicitly copying owners. */
TEST(Operators_ClassArithmetic, value_operand_moves) {
  EXPECT_THROW(executeString(R"(
    /** Owns a value passed by move. */
    class Number {
      var value: i32 = 21;
      /** Consumes the right operand. */
      public const method __add__(other: Number) i32 { return this.value + other.value; }
    }
    /** Attempts to reuse a consumed right operand. */
    function main() i32 {
      var a = Number(); var b = Number(); var total = a + b;
      return total + b.value;
    }
  )"), SunError);
}

/** Checks that operator syntax cannot bypass an unsafe method boundary. */
TEST(Operators_ClassArithmetic, unsafe_hook_requires_unsafe_block) {
  EXPECT_THROW(executeString(R"(
    /** Supplies an unsafe operation. */
    class Number {
      /** Requires the caller to opt into unsafe execution. */
      public const unsafe method __add__(value: i32) i32 { return value; }
    }
    /** Attempts an unsafe operator in safe code. */
    function main() i32 { var n = Number(); return n + 42; }
  )"), SunError);
}
