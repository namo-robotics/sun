// tests/functions/generic/test_constraints.cpp - Tests for generic type
// parameter constraints: <T: _Numeric>, <T: IError>, <F: _Lambda>
//
// A constraint asks the same question `_is<T>` asks in a function body, only
// at the signature instead, so these tests also pin that the two vocabularies
// stay one vocabulary.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "driver/execution_utils.h"

// -------------------------------------------------------------------
// Built-in traits
// -------------------------------------------------------------------

TEST(Functions_Generic_Constraints, numeric_accepts_integer) {
  auto value = executeString(R"(
    function twice<T: _Numeric>(x: T) T { return x + x; }
    function main() i32 { return twice<i32>(21); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Functions_Generic_Constraints, numeric_accepts_float) {
  auto value = executeString(R"(
    function twice<T: _Numeric>(x: T) T { return x + x; }
    function main() i32 {
      var d: f64 = twice<f64>(1.5);
      return _convert<i32>(d);
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Functions_Generic_Constraints, numeric_rejects_bool) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    function twice<T: _Numeric>(x: T) T { return x; }
    function main() i32 { var b = twice<bool>(true); return 0; }
  )"),
      "type argument 'bool' does not satisfy constraint '_Numeric' on type "
      "parameter 'T' of generic function 'twice'");
}

TEST(Functions_Generic_Constraints, numeric_rejects_class) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Point { init() {} }
    function twice<T: _Numeric>(x: T) i32 { return 0; }
    function main() i32 { var p = Point(); return twice(p); }
  )"),
                                "does not satisfy constraint '_Numeric'");
}

TEST(Functions_Generic_Constraints, integer_rejects_float) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function bits<T: _Integer>(x: T) i32 { return 0; }
    function main() i32 { return bits<f64>(1.5); }
  )"),
                                "does not satisfy constraint '_Integer'");
}

TEST(Functions_Generic_Constraints, signed_rejects_unsigned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function negate<T: _Signed>(x: T) i32 { return 0; }
    function main() i32 { return negate<u32>(1); }
  )"),
                                "does not satisfy constraint '_Signed'");
}

TEST(Functions_Generic_Constraints, signed_accepts_signed) {
  auto value = executeString(R"(
    function pick<T: _Signed>(x: T) i32 { return 5; }
    function main() i32 { return pick<i64>(1); }
  )");
  EXPECT_EQ(value, 5);
}

// -------------------------------------------------------------------
// Interface constraints
// -------------------------------------------------------------------

TEST(Functions_Generic_Constraints, interface_accepts_implementor) {
  auto value = executeString(R"(
    interface IShape { public method area() i32; }
    class Square implements IShape {
      var side: i32;
      init(s: i32) { this.side = s; }
      public method area() i32 { return this.side * this.side; }
    }
    function tag<T: IShape>(s: ref T) i32 { return 6; }
    function main() i32 { var sq = Square(3); return tag(sq); }
  )");
  EXPECT_EQ(value, 6);
}

TEST(Functions_Generic_Constraints, interface_rejects_non_implementor) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    interface IShape { public method area() i32; }
    class Dot { init() {} }
    function tag<T: IShape>(s: ref T) i32 { return 0; }
    function main() i32 { var d = Dot(); return tag(d); }
  )"),
      "type argument 'Dot' does not satisfy constraint 'IShape' on type "
      "parameter 'T' of generic function 'tag'");
}

// -------------------------------------------------------------------
// The _Lambda constraint
// -------------------------------------------------------------------

TEST(Functions_Generic_Constraints, lambda_accepts_a_closure) {
  auto value = executeString(R"(
    function takes<F: _Lambda>(f: F) i32 { return 37; }
    function main() i32 {
      var g = (x: i32) => i32 { return x + 1; };
      return takes(g);
    }
  )");
  EXPECT_EQ(value, 37);
}

TEST(Functions_Generic_Constraints, lambda_rejects_a_number) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    function takes<F: _Lambda>(f: F) i32 { return 0; }
    function main() i32 { return takes(5); }
  )"),
      "type argument 'i32' does not satisfy constraint '_Lambda' on type "
      "parameter 'F' of generic function 'takes'");
}

// A constraint and `_is<T>` share one vocabulary, so `_Lambda` is a trait in
// both positions: at a signature, and in a body.
TEST(Functions_Generic_Constraints, is_lambda_trait_is_true_for_a_closure) {
  auto value = executeString(R"(
    function check(f: (i32) => i32) bool { return _is<_Lambda>(f); }
    function main() i32 {
      var g = (x: i32) => i32 { return x; };
      if (check(g)) { return 1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Functions_Generic_Constraints, is_lambda_trait_is_false_for_a_number) {
  auto value = executeString(R"(
    function check(x: i32) bool { return _is<_Lambda>(x); }
    function main() i32 { if (check(5)) { return 1; } return 0; }
  )");
  EXPECT_EQ(value, 0);
}

// -------------------------------------------------------------------
// An interface constraint makes its members reachable in the body
// -------------------------------------------------------------------

// Whatever T turns out to be, it implements the interface, so the interface's
// methods can be called on a value of type T while the template is analyzed —
// before any specialization exists.
TEST(Functions_Generic_Constraints, interface_method_callable_in_body) {
  auto value = executeString(R"(
    interface IShape { public method area() i32; }
    class Square implements IShape {
      var side: i32;
      init(s: i32) { this.side = s; }
      public method area() i32 { return this.side * this.side; }
    }
    function measure<T: IShape>(s: ref T) i32 { return s.area(); }
    function main() i32 { var sq = Square(6); return measure(sq); }
  )");
  EXPECT_EQ(value, 36);
}

TEST(Functions_Generic_Constraints, interface_field_readable_in_body) {
  auto value = executeString(R"(
    interface INamed { public var tag: i32; }
    class Thing implements INamed {
      public var tag: i32;
      init(t: i32) { this.tag = t; }
    }
    function read<T: INamed>(x: ref T) i32 { return x.tag; }
    function main() i32 { var t = Thing(10); return read(t); }
  )");
  EXPECT_EQ(value, 10);
}

TEST(Functions_Generic_Constraints, interface_constraint_on_generic_class_body) {
  auto value = executeString(R"(
    interface IShape { public method area() i32; }
    class Square implements IShape {
      var side: i32;
      init(s: i32) { this.side = s; }
      public method area() i32 { return this.side * this.side; }
    }
    class Holder<T: IShape> {
      var item: T;
      init(i: T) { this.item = i; }
      public method measure() i32 { return this.item.area(); }
    }
    function main() i32 {
      var h = Holder<Square>(Square(5));
      return h.measure();
    }
  )");
  EXPECT_EQ(value, 25);
}

TEST(Functions_Generic_Constraints, interface_constraint_on_generic_method) {
  auto value = executeString(R"(
    interface IShape { public method area() i32; }
    class Square implements IShape {
      var side: i32;
      init(s: i32) { this.side = s; }
      public method area() i32 { return this.side * this.side; }
    }
    class Ruler {
      init() {}
      public method measure<T: IShape>(s: ref T) i32 { return s.area(); }
    }
    function main() i32 {
      var r = Ruler();
      var sq = Square(4);
      return r.measure(sq);
    }
  )");
  EXPECT_EQ(value, 16);
}

// A member the interface does not declare is still an error, and the message
// says which interface was consulted.
TEST(Functions_Generic_Constraints, member_not_on_the_constraint_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    interface IShape { public method area() i32; }
    class Square implements IShape {
      var side: i32;
      init(s: i32) { this.side = s; }
      public method area() i32 { return this.side * this.side; }
    }
    function measure<T: IShape>(s: ref T) i32 { return s.perimeter(); }
    function main() i32 { var sq = Square(6); return measure(sq); }
  )"),
      "Unknown member 'perimeter' on type parameter 'T', which is constrained "
      "to interface 'IShape'");
}

// A trait says which types are allowed, not what members they carry.
TEST(Functions_Generic_Constraints, trait_constraint_promises_no_members) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    function twice<T: _Numeric>(x: T) i32 { return x.area(); }
    function main() i32 { return twice(1); }
  )"),
      "its constraint '_Numeric' is a type trait, which promises no members");
}

TEST(Functions_Generic_Constraints, unconstrained_parameter_has_no_members) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function f<T>(x: T) i32 { return x.area(); }
    function main() i32 { return f(1); }
  )"),
                                "unconstrained type parameter 'T'");
}

// -------------------------------------------------------------------
// Interaction with unconstrained parameters
// -------------------------------------------------------------------

TEST(Functions_Generic_Constraints, unconstrained_parameter_accepts_anything) {
  auto value = executeString(R"(
    function pick<T>(x: T) i32 { return 9; }
    function main() i32 { var b: bool = true; return pick(b); }
  )");
  EXPECT_EQ(value, 9);
}

TEST(Functions_Generic_Constraints, only_some_parameters_constrained) {
  auto value = executeString(R"(
    function pair<T, U: _Numeric>(a: T, b: U) i32 { return 11; }
    function main() i32 { var f: bool = false; return pair(f, 3); }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Functions_Generic_Constraints,
     second_parameter_constraint_is_checked_too) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function pair<T, U: _Numeric>(a: T, b: U) i32 { return 0; }
    function main() i32 { var f: bool = false; return pair(1, f); }
  )"),
                                "on type parameter 'U'");
}

// -------------------------------------------------------------------
// Constraints on other generic declarations
// -------------------------------------------------------------------

TEST(Functions_Generic_Constraints, generic_class_constraint_accepts) {
  auto value = executeString(R"(
    class Box<T: _Numeric> {
      var value: T;
      init(v: T) { this.value = v; }
      public method get() T { return this.value; }
    }
    function main() i32 { var b = Box<i32>(12); return b.get(); }
  )");
  EXPECT_EQ(value, 12);
}

TEST(Functions_Generic_Constraints, generic_class_constraint_rejects) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    class Box<T: _Numeric> {
      var value: T;
      init(v: T) { this.value = v; }
    }
    function main() i32 { var b = Box<bool>(true); return 0; }
  )"),
      "type argument 'bool' does not satisfy constraint '_Numeric' on type "
      "parameter 'T' of generic class 'Box'");
}

TEST(Functions_Generic_Constraints, generic_method_constraint_rejects) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class F {
      init() {}
      public method make<T: _Numeric>() i32 { return 0; }
    }
    function main() i32 { var f = F(); return f.make<bool>(); }
  )"),
                                "does not satisfy constraint '_Numeric'");
}

TEST(Functions_Generic_Constraints, generic_enum_constraint_rejects) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    enum Maybe<T: _Numeric> { Some(T), None }
    function main() i32 {
      var m: Maybe<bool> = Maybe.Some(true);
      return 0;
    }
  )"),
                                "does not satisfy constraint '_Numeric'");
}

// -------------------------------------------------------------------
// Deferred checking inside template bodies
// -------------------------------------------------------------------

// A constraint on an inner generic is checked when the outer one is
// specialized with a real type, not while the outer template body is analyzed
// with T still standing for itself.
TEST(Functions_Generic_Constraints, checked_when_outer_generic_specializes) {
  auto value = executeString(R"(
    function inner<T: _Numeric>(x: T) T { return x + x; }
    function outer<U>(x: U) U { return inner(x); }
    function main() i32 { return outer<i32>(4); }
  )");
  EXPECT_EQ(value, 8);
}

TEST(Functions_Generic_Constraints, outer_specialization_surfaces_violation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function inner<T: _Numeric>(x: T) i32 { return 0; }
    function outer<U>(x: U) i32 { return inner(x); }
    function main() i32 { var b: bool = true; return outer(b); }
  )"),
                                "does not satisfy constraint '_Numeric'");
}

// The constraint carries its own source span, so the caret lands on the
// constraint rather than somewhere in the parameter list.
TEST(Functions_Generic_Constraints, diagnostic_points_at_the_constraint) {
  try {
    executeString(R"(
    function twice<T: _Numeric>(x: T) T { return x + x; }
    function main() i32 { var b = twice<bool>(true); return 0; }
  )");
    FAIL() << "Expected SunError to be thrown";
  } catch (const SunError& e) {
    // Column 23 is where `_Numeric` starts on line 2 of the snippet above.
    EXPECT_NE(std::strstr(e.what(), ":2:23"), nullptr)
        << "Expected the caret at the constraint. Actual: " << e.what();
  }
}

// -------------------------------------------------------------------
// Malformed constraints
// -------------------------------------------------------------------

TEST(Functions_Generic_Constraints, missing_constraint_after_colon_is_error) {
  EXPECT_THROW(executeString(R"(
    function f<T: >(x: T) i32 { return 0; }
    function main() i32 { return f(1); }
  )"),
               SunError);
}
