// tests/control_flow/test_match.cpp - Tests for match expressions

#include <gtest/gtest.h>

#include "execution_utils.h"

// ============================================================================
// Basic match expression with integer patterns
// ============================================================================

TEST(ControlFlow_Match, BasicIntegerMatch) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 2;
      return match x {
        1 => 10,
        2 => 20,
        3 => 30,
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(ControlFlow_Match, MatchFirstArm) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 1;
      return match x {
        1 => 100,
        2 => 200,
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(ControlFlow_Match, MatchLastArm) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 3;
      return match x {
        1 => 100,
        2 => 200,
        3 => 300,
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 300);
}

TEST(ControlFlow_Match, MatchWildcard) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 99;
      return match x {
        1 => 10,
        2 => 20,
        _ => 999
      };
    }
  )");
  EXPECT_EQ(value, 999);
}

// ============================================================================
// Match as expression (returns value)
// ============================================================================

TEST(ControlFlow_Match, MatchAsExpression) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 2;
      var result = match x {
        1 => 10,
        2 => 20,
        _ => 0
      };
      return result;
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(ControlFlow_Match, MatchInBinaryExpression) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 1;
      var y = 10 + match x {
        1 => 5,
        _ => 0
      };
      return y;
    }
  )");
  EXPECT_EQ(value, 15);
}

// ============================================================================
// Match with block bodies
// ============================================================================

TEST(ControlFlow_Match, MatchWithBlockBody) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 2;
      return match x {
        1 => {
          var a = 10;
          return a + 5;
        },
        2 => {
          var b = 20;
          return b + 5;
        },
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 25);
}

// ============================================================================
// Match with function calls
// ============================================================================

TEST(ControlFlow_Match, MatchOnFunctionResult) {
  auto value = executeString(R"(
    function getValue() i32 {
      return 3;
    }

    function main() i32 {
      return match getValue() {
        1 => 100,
        2 => 200,
        3 => 300,
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 300);
}

TEST(ControlFlow_Match, MatchCallsFunctionInArm) {
  auto value = executeString(R"(
    function double(n: i32) i32 {
      return n * 2;
    }

    function main() i32 {
      var x = 5;
      return match x {
        1 => double(10),
        5 => double(25),
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 50);
}

// ============================================================================
// Nested match expressions
// ============================================================================

TEST(ControlFlow_Match, NestedMatch) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 1;
      var y = 2;
      return match x {
        1 => match y {
          1 => 11,
          2 => 12,
          _ => 10
        },
        2 => 20,
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 12);
}

// ============================================================================
// Match with different integer sizes
// ============================================================================

TEST(ControlFlow_Match, MatchI64) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i64 = 2;
      var result: i64 = match x {
        1 => 100,
        2 => 200,
        _ => 0
      };
      return result;
    }
  )");
  EXPECT_EQ(value, 200);
}

// ============================================================================
// Match without wildcard (last arm matches)
// ============================================================================

TEST(ControlFlow_Match, MatchWithoutWildcard) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 2;
      return match x {
        1 => 10,
        2 => 20
      };
    }
  )");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Match with boolean patterns
// ============================================================================

TEST(ControlFlow_Match, MatchBoolean) {
  auto value = executeString(R"(
    function main() i32 {
      var flag = true;
      return match flag {
        true => 1,
        false => 0,
        _ => 99
      };
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(ControlFlow_Match, MatchBooleanFalse) {
  auto value = executeString(R"(
    function main() i32 {
      var flag = false;
      return match flag {
        true => 1,
        false => 0,
        _ => 99
      };
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Match with enum values
// ============================================================================

TEST(ControlFlow_Match, MatchEnum) {
  auto value = executeString(R"(
    enum Color { Red, Green, Blue }

    function main() i32 {
      var c: Color = Color.Green;
      return match c {
        Color.Red => 1,
        Color.Green => 2,
        Color.Blue => 3,
        _ => 0
      };
    }
  )");
  EXPECT_EQ(value, 2);
}

// ============================================================================
// Match with trailing comma
// ============================================================================

TEST(ControlFlow_Match, TrailingComma) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 1;
      return match x {
        1 => 10,
        2 => 20,
        _ => 0,
      };
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// Match as statement (ignoring result)
// ============================================================================

TEST(ControlFlow_Match, MatchAsStatement) {
  auto value = executeString(R"(
    function main() i32 {
      var x = 2;
      var result = 0;
      match x {
        1 => { result = 10; },
        2 => { result = 20; },
        _ => { result = 99; }
      };
      return result;
    }
  )");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Return from within match block
// ============================================================================

TEST(ControlFlow_Match, ReturnFromMatchBlock) {
  // Return from match arm exits the function immediately
  auto value = executeString(R"(
    function main() i32 {
      var x = 2;
      match x {
        1 => { return 10; },
        2 => { return 20; },
        _ => { return 99; }
      };
      return 0;
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(ControlFlow_Match, ReturnFromMatchBlockFirst) {
  // Return from first arm
  auto value = executeString(R"(
    function main() i32 {
      var x = 1;
      match x {
        1 => { return 100; },
        2 => { return 200; },
        _ => { return 999; }
      };
      return 0;
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(ControlFlow_Match, ReturnFromMatchBlockWildcard) {
  // Return from wildcard arm
  auto value = executeString(R"(
    function main() i32 {
      var x = 42;
      match x {
        1 => { return 10; },
        2 => { return 20; },
        _ => { return 999; }
      };
      return 0;
    }
  )");
  EXPECT_EQ(value, 999);
}

// ============================================================================
// No match - falls through to code after match
// ============================================================================

TEST(ControlFlow_Match, NoMatchFallthrough) {
  // When no arm matches and there's no wildcard, execution continues after
  // match
  auto value = executeString(R"(
    function main() i32 {
      var x = 99;
      match x {
        1 => { return 10; },
        2 => { return 20; },
        3 => { return 30; }
      };
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(ControlFlow_Match, NoMatchFallthroughWithSideEffects) {
  // Side effects don't happen when no arm matches
  auto value = executeString(R"(
    function main() i32 {
      var x = 100;
      var result = 5;
      match x {
        1 => { result = 10; },
        2 => { result = 20; }
      };
      return result;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(ControlFlow_Match, NoMatchContinuesExecution) {
  // Code after match runs when no arm matches
  auto value = executeString(R"(
    function main() i32 {
      var x = 50;
      var sum = 0;
      match x {
        1 => { sum = sum + 1; },
        2 => { sum = sum + 2; }
      };
      sum = sum + 100;
      return sum;
    }
  )");
  EXPECT_EQ(value, 100);
}

// A non-void method whose body ends in a match where every arm returns or
// throws has no fall-through; codegen must still terminate the tail block.
TEST(ControlFlow_Match, method_ending_in_fully_terminating_match) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class NotANumber implements IError {
      function init() {}
      function code() i32 { return 7; }
      function message() static_ptr<u8> { return "not a number"; }
    }

    enum Value {
      Int(i64),
      Float(f64),
      Empty
    }

    class Holder {
      var v: Value;
      function init(v: Value) { this.v = v; }
      function as_f64() f64, IError {
        match this.v {
          Value.Int(x) => { return _convert<f64>(x); },
          Value.Float(x) => { return x; },
          _ => { throw NotANumber(); }
        };
      }
    }

    function main() i32 {
      var a = Holder(Value.Int(4));
      var b = Holder(Value.Empty);
      var total: f64 = 0.0;
      try {
        total = a.as_f64();
        total = total + b.as_f64();
      } catch (e: IError) {
        return _convert<i32>(total) + e.code();
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 11);
}

// A match arm whose body is a void statement contributes no value.
TEST(ControlFlow_Match, statement_arms_with_void_calls) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    enum Value {
      Items(Vec<i64>),
      Empty
    }

    class Holder {
      var v: Value;
      function init(v: Value) { this.v = v; }
      function push(x: i64) void {
        match this.v {
          Value.Items(items) => { items.push(x); },
          Value.Empty => { }
        };
      }
      function count() i64 {
        return match this.v {
          Value.Items(items) => items.size(),
          Value.Empty => 0
        };
      }
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      var h = Holder(Value.Items(Vec<i64>(alloc, 2)));
      h.push(1);
      h.push(2);
      h.push(3);
      var e = Holder(Value.Empty);
      e.push(9);
      return _convert<i32>(h.count() + e.count());
    }
  )");
  EXPECT_EQ(value, 3);
}
