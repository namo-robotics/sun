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

using sun::driver::executeString;
using sun::support::SunError;

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

// -------------------------------------------------------------------
// The _Callable constraint: lambdas and named functions alike
// -------------------------------------------------------------------

// Called the way spawn calls its callee: through the pack its parameters
// describe. (A direct `f()` on a parameter of type F is not typed yet; only
// the `f(args...)` shape is.)
TEST(Functions_Generic_Constraints, callable_accepts_a_named_function) {
  auto value = executeString(R"(
    function run<F: _Callable>(f: F, args...: _params_of<F>) i32 {
      return f(args...);
    }
    function seven() i32 { return 7; }
    function main() i32 {
      var lambda = (n: i32) => i32 { return n + 20; };
      return run(seven) + run(lambda, 10);
    }
  )");
  EXPECT_EQ(value, 37);
}

// The same calls with F written out: a function type satisfies _Callable
// whether it was inferred or spelled (issue #193).
TEST(Functions_Generic_Constraints, callable_accepts_a_written_function_type) {
  auto value = executeString(R"(
    function run<F: _Callable>(f: F, args...: _params_of<F>) i32 {
      return f(args...);
    }
    function seven() i32 { return 7; }
    function twice(n: i32) i32 { return n * 2; }
    function main() i32 {
      return run<function () i32>(seven) + run<function (i32) i32>(twice, 15);
    }
  )");
  EXPECT_EQ(value, 37);
}

TEST(Functions_Generic_Constraints, callable_rejects_a_number) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function run<F: _Callable>(f: F) i32 { return 0; }
    function main() i32 { return run(42); }
  )"),
                                "does not satisfy constraint '_Callable'");
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

TEST(Functions_Generic_Constraints,
     interface_constraint_on_generic_class_body) {
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

// A generic field keeps the caller's interface constraint during analysis.
TEST(Functions_Generic_Constraints, boxed_interface_method) {
  for (const auto* call : {"call_boxed<Impl>(b, 41)", "call_boxed(b, 41)"}) {
    SCOPED_TRACE(call);
    EXPECT_EQ(executeString(std::string(R"(
      interface IHandler { public method handle(x: i32) i32; }
      class Impl implements IHandler {
        init() {}
        public method handle(x: i32) i32 { return x + 1; }
      }
      class Box<H: IHandler> {
        var inner: H;
        init(inner: H) { this.inner = inner; }
        public method go(x: i32) i32 { return this.inner.handle(x); }
      }
      function call_boxed<H: IHandler>(b: ref Box<H>, x: i32) i32 {
        return b.inner.handle(x);
      }
      function main() i32 {
        var b = Box<Impl>(Impl());
        return )") + call + "; }"),
              42);
  }
}

TEST(Functions_Generic_Constraints, boxed_nested_interface_field) {
  EXPECT_EQ(executeString(R"(
    interface INamed { public var tag: i32; }
    class Item implements INamed {
      public var tag: i32;
      init() { this.tag = 42; }
    }
    class Box<T> {
      var inner: T;
      init(inner: T) { this.inner = inner; }
    }
    function read<H: INamed>(b: ref Box<Box<H>>) i32 {
      return b.inner.inner.tag;
    }
    function main() i32 {
      var b = Box<Box<Item>>(Box<Item>(Item()));
      return read(b);
    }
  )"),
            42);
}

// Cached Box<H> fields must not borrow another function's constraint.
TEST(Functions_Generic_Constraints, boxed_constraints_are_local_to_function) {
  const std::string first = R"(
    function first<H: IFirst>(b: ref Box<H>) i32 { return b.inner.first(); }
  )";
  const std::string second = R"(
    function second<H: ISecond>(b: ref Box<H>) i32 { return b.inner.second(); }
  )";
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    EXPECT_EQ(executeString(std::string(R"(
      interface IFirst { public method first() i32; }
      interface ISecond { public method second() i32; }
      class A implements IFirst {
        init() {}
        public method first() i32 { return 20; }
      }
      class B implements ISecond {
        init() {}
        public method second() i32 { return 22; }
      }
      class Box<T> {
        var inner: T;
        init(inner: T) { this.inner = inner; }
      }
    )") + (reverse ? second + first : first + second) +
                            R"(
      function main() i32 {
        var a = Box<A>(A());
        var b = Box<B>(B());
        return first(a) + second(b);
      }
    )"),
              42);
  }
}

TEST(Functions_Generic_Constraints, boxed_unconstrained_member_is_rejected) {
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    const std::string constrained = R"(
      function valid<H: IHandler>(b: ref Box<H>) i32 {
        return b.inner.handle();
      }
    )";
    const std::string unconstrained = R"(
      function invalid<H>(b: ref Box<H>) i32 { return b.inner.handle(); }
    )";
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        executeString(std::string(R"(
      interface IHandler { public method handle() i32; }
      class Box<T> { var inner: T; init(inner: T) { this.inner = inner; } }
    )") +
                      (reverse ? unconstrained + constrained
                               : constrained + unconstrained) +
                      "function main() i32 { return 0; }"),
        "unconstrained type parameter 'H'");
  }
}

TEST(Functions_Generic_Constraints,
     boxed_missing_interface_member_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    interface IHandler { public method handle() i32; }
    class Box<T> { var inner: T; init(inner: T) { this.inner = inner; } }
    function invalid<H: IHandler>(b: ref Box<H>) i32 {
      return b.inner.missing();
    }
    function main() i32 { return 0; }
  )"),
      "Unknown member 'missing' on type parameter 'H', which is constrained "
      "to interface 'IHandler'");
}

TEST(Functions_Generic_Constraints, boxed_nonimplementor_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IHandler { public method handle() i32; }
    class Other { init() {} }
    class Box<T> { var inner: T; init(inner: T) { this.inner = inner; } }
    function call_boxed<H: IHandler>(b: ref Box<H>) i32 {
      return b.inner.handle();
    }
    function main() i32 {
      var b = Box<Other>(Other());
      return call_boxed(b);
    }
  )"),
                                "does not satisfy constraint 'IHandler'");
}

// Issue #213: module-local constraints and calls use the declaration's scope.
TEST(Functions_Generic_Constraints, module_interface_constraints) {
  for (const auto* call : {"call<Impl>(item, 41)", "call(item, 41)"}) {
    SCOPED_TRACE(call);
    EXPECT_EQ(executeString(std::string(R"(
      /** Groups constrained handlers. */
      module spike {
        /** Handles an integer. */
        public interface IHandler {
          /** Transforms the input. */
          public method handle(x: i32) i32;
        }
        /** Increments the input. */
        public class Impl implements IHandler {
          init() {}
          /** Returns the incremented input. */
          public method handle(x: i32) i32 { return x + 1; }
        }
        /** Stores a handler with static dispatch. */
        public class Box<H: IHandler> {
          var inner: H;
          init(inner: H) { this.inner = inner; }
          /** Passes the input to the handler. */
          public method go(x: i32) i32 { return this.inner.handle(x); }
        }
        function call<H: IHandler>(item: ref H, x: i32) i32 {
          return item.handle(x);
        }
        /** Exercises module-local generic calls. */
        public function run() i32 {
          var b = Box<Impl>(Impl());
          var item = Impl();
          return b.go(41) + )") +
                            call + R"(;
        }
      }
      function main() i32 { return spike.run(); }
    )"),
              84);
  }
}

TEST(Functions_Generic_Constraints, module_field_uses_file_scope_interface) {
  EXPECT_EQ(executeString(R"(
    interface IHandler {
      /** Transforms the input. */
      public method handle(x: i32) i32;
    }
    /** Groups dynamically dispatched handlers. */
    module spike {
      class Impl implements IHandler {
        init() {}
        /** Returns the incremented input. */
        public method handle(x: i32) i32 { return x + 1; }
      }
      class Box<H: IHandler> {
        var inner: IHandler;
        init(inner: H) { this.inner = inner; }
        /** Passes the input through the interface field. */
        public method go(x: i32) i32 { return this.inner.handle(x); }
      }
      /** Exercises the interface field. */
      public function run() i32 {
        var b = Box<Impl>(Impl());
        return b.go(41);
      }
    }
    function main() i32 { return spike.run(); }
  )"),
            42);
}

TEST(Functions_Generic_Constraints, constraint_only_overloads_are_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function run<F: _Lambda>(f: F, args...: _params_of<F>) i32 { return 1; }
    function run<F: _Function>(f: F, args...: _params_of<F>) i32 { return 2; }
    function main() i32 { return 0; }
  )"),
                                "Generic function 'run' is already declared in "
                                "this scope; generic function overloads are "
                                "not supported");
}

/** Keeps test fixtures and helpers local to this source file. */
namespace {
const std::string genericHandler = R"(
  interface IHandler<Self> {
    /** Copies the handler explicitly. */
    public const method clone() Self;
    /** Returns the stored value. */
    public const method value() i32;
  }
  class Handler implements IHandler<Handler> {
    var n: i32;
    init(n: i32) { this.n = n; }
    /** Copies the stored value. */
    public const method clone() Handler { return Handler(this.n); }
    /** Returns the stored value. */
    public const method value() i32 { return this.n; }
  }
)";
}

TEST(Functions_Generic_Constraints, generic_interface_self_clone) {
  for (const auto* closing : {">>", "> >"}) {
    SCOPED_TRACE(closing);
    EXPECT_EQ(executeString(genericHandler + "function read<H: IHandler<H" +
                            closing + R"((item: const ref H) i32 {
          var copied: H = item.clone();
          return copied.value();
        }
        function main() i32 {
          var item = Handler(42);
          return read(item);
        }
      )"),
              42);
  }
}

TEST(Functions_Generic_Constraints, generic_interface_server_class) {
  EXPECT_EQ(executeString(genericHandler + R"(
    class Server<H: IHandler<H>> {
      var handler: H;
      init(handler: H) { this.handler = handler; }
      /** Reads a copied handler. */
      public const method read() i32 {
        var copied = this.handler.clone();
        return copied.value();
      }
    }
    function main() i32 {
      var server = Server<Handler>(Handler(42));
      return server.read();
    }
  )"),
            42);
}

TEST(Functions_Generic_Constraints,
     generic_interface_forward_parameter_and_field) {
  EXPECT_EQ(executeString(R"(
    interface IValue<T, Self> {
      public var value: T;
      /** Copies the value container. */
      public const method clone() Self;
    }
    class Value implements IValue<i32, Value> {
      public var value: i32;
      init(value: i32) { this.value = value; }
      /** Copies the value container. */
      public const method clone() Value { return Value(this.value); }
    }
    function read<H: IValue<T, H>, T>(item: const ref H) T {
      var copied: H = item.clone();
      return copied.value;
    }
    function main() i32 {
      var item = Value(42);
      return read<Value, i32>(item);
    }
  )"),
            42);
}

TEST(Functions_Generic_Constraints, generic_interface_nested_arguments) {
  EXPECT_EQ(executeString(R"(
    class Box<T> {
      public var value: T;
      init(value: T) { this.value = value; }
    }
    interface IBox<T> { public var box: T; }
    class Item implements IBox<Box<i32>> {
      public var box: Box<i32>;
      init() { this.box = Box<i32>(42); }
    }
    function read<H: IBox<Box<T>>, T>(item: const ref H) T {
      return item.box.value;
    }
    function main() i32 { var item = Item(); return read<Item, i32>(item); }
  )"),
            42);
}

TEST(Functions_Generic_Constraints,
     generic_interface_method_uses_class_parameter) {
  EXPECT_EQ(executeString(R"(
    interface IValue<T> { public var value: T; }
    class Value implements IValue<i32> { public var value: i32; }
    class Reader<T> {
      init() {}
      /** Reads an implementation of the class's value type. */
      public method read<H: IValue<T>>(item: const ref H) T { return item.value; }
    }
    function main() i32 {
      var reader = Reader<i32>();
      var item: Value = { value: 42 };
      return reader.read<Value>(item);
    }
  )"),
            42);
}

TEST(Functions_Generic_Constraints, generic_interface_enum_and_interface) {
  EXPECT_EQ(executeString(genericHandler + R"(
    enum Held<H: IHandler<H>> { Some(H), None }
    interface IServer<H: IHandler<H>> {
      /** Reads the server. */
      public method read() i32;
    }
    class Server implements IServer<Handler> {
      init() {}
      /** Returns the server value. */
      public method read() i32 { return 42; }
    }
    function main() i32 {
      var held: Held<Handler> = Held.Some(Handler(42));
      var server = Server();
      return server.read();
    }
  )"),
            42);
}

TEST(Functions_Generic_Constraints, generic_interface_deferred_specialization) {
  EXPECT_EQ(executeString(genericHandler + R"(
    function inner<H: IHandler<H>>(item: const ref H) i32 {
      return item.value();
    }
    function outer<T: IHandler<T>>(item: const ref T) i32 { return inner<T>(item); }
    function main() i32 { var item = Handler(42); return outer(item); }
  )"),
            42);
}

TEST(Functions_Generic_Constraints, generic_interface_wrong_specialization) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IValue<T> { public var value: T; }
    class Value implements IValue<i32> { public var value: i32; }
    function read<H: IValue<i64>>(item: const ref H) i64 { return item.value; }
    function main() i32 { var item: Value = { value: 42 }; read(item); return 0; }
  )"),
                                "does not satisfy constraint 'IValue<i64>'");
}

TEST(Functions_Generic_Constraints, generic_interface_missing_implementation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(genericHandler + R"(
    class Other { init() {} }
    function read<H: IHandler<H>>(item: const ref H) i32 { return item.value(); }
    function main() i32 { var item = Other(); return read(item); }
  )"),
                                "does not satisfy constraint 'IHandler<H>'");
}

TEST(Functions_Generic_Constraints, generic_interface_invalid_requirements) {
  for (const auto& [constraint, diagnostic] :
       {std::pair{"IHandler<i32, i64>", "expects 1 type arguments, got 2"},
        std::pair{"Missing<i32>", "Unknown generic type 'Missing'"},
        std::pair{"_Numeric<i32>", "must name an interface"}}) {
    SCOPED_TRACE(constraint);
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        executeString(genericHandler + "function read<H: " + constraint +
                      R"(>(item: const ref H) i32 { return 0; }
        function main() i32 { var item = Handler(42); return read(item); }
      )"),
        diagnostic);
  }
}

TEST(Functions_Generic_Constraints, generic_interface_malformed_arguments) {
  for (const auto* constraint :
       {"IHandler<>", "IHandler<i32,>", "IHandler<i32"}) {
    SCOPED_TRACE(constraint);
    EXPECT_THROW(executeString(std::string("class Server<H: ") + constraint +
                               "> {} function main() i32 { return 0; }"),
                 SunError);
  }
}

TEST(Functions_Generic_Constraints, generic_interface_deferred_violation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(genericHandler + R"(
    class Other { init() {} }
    function inner<H: IHandler<H>>(item: const ref H) i32 { return item.value(); }
    function outer<T>(item: const ref T) i32 { return inner<T>(item); }
    function main() i32 { var item = Other(); return outer(item); }
  )"),
                                "does not satisfy constraint 'IHandler<H>'");
}

TEST(Functions_Generic_Constraints,
     generic_interface_rejected_on_other_declarations) {
  for (const auto* program : {
           R"(
        class Gate<H: IValue<i64>> { init() {} }
        function main() i32 { var gate = Gate<Item>(); return 0; }
      )",
           R"(
        enum Held<H: IValue<i64>> { Some(H), None }
        function main() i32 { var held: Held<Item> = Held.None; return 0; }
      )",
           R"(
        interface IGate<H: IValue<i64>> { public var value: i32; }
        class Gate implements IGate<Item> { public var value: i32; }
        function main() i32 { return 0; }
      )"}) {
    SCOPED_TRACE(program);
    EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(std::string(R"(
      interface IValue<T> { public var value: T; }
      class Item implements IValue<i32> { public var value: i32; }
    )") + program),
                                  "does not satisfy constraint 'IValue<i64>'");
  }
}
