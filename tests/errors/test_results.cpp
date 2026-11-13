/** Verifies native exception integration and ordinary enum ownership contracts.
 */
#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>

#include <functional>
#include <sstream>

#include "ast/ast_children.h"
#include "ast/exception_ast.h"
#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "parsing/formatter.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"

using sun::driver::executeString;

/** Generic handlers can cover payload types absent from one specialization. */
TEST(Errors_Integration, generic_typed_matches_allow_absent_payloads) {
  EXPECT_EQ(executeString(R"(
    /** Identifies the first kind of failure. */
    class First {}
    /** Identifies the second kind of failure. */
    class Second {}
    /** Carries only the first failure kind. */
    enum One { Ok, Failed(First) }
    /** Carries only the second failure kind. */
    enum Two { Ok, Failed(Second) }
    /** Classifies either result while preserving a success fallback. */
    function classify<R>(value: R) i32 {
      return match value {
        (error: const ref First) => 1,
        (error: const ref Second) => 2,
        _ => 0
      };
    }
    /** Exercises both specializations and their success variants. */
    function main() i32 {
      return classify(One.Failed(First())) + classify(Two.Failed(Second()))
        + classify(One.Ok) + classify(Two.Ok);
    }
  )"), 3);
}

/** Concrete matches still diagnose a payload type that cannot occur. */
TEST(Errors_Integration, concrete_typed_matches_reject_absent_payloads) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Identifies a failure absent from the result. */
    class Failure {}
    /** Has no payload to bind. */
    enum Outcome { Ok, Failed }
    /** Rejects the impossible typed arm. */
    function main() i32 {
      return match Outcome.Ok {
        (error: const ref Failure) => 1,
        _ => 0
      };
    }
  )"), "Typed match pattern matches no variant payloads");
}

/** Obsolete wire nodes are rejected before they enter the compiler AST. */
TEST(Errors_Integration, rejects_serialized_exception_nodes) {
  sun::serialization::ASTDeserializer deserializer;
  sun::proto::ast::ASTNode throwing;
  throwing.mutable_throw_expr();
  EXPECT_SUN_ERROR_WITH_MESSAGE(deserializer.deserialize(throwing),
                                "Legacy throw nodes are unsupported");
  sun::proto::ast::ASTNode catching;
  catching.mutable_try_catch();
  EXPECT_SUN_ERROR_WITH_MESSAGE(deserializer.deserialize(catching),
                                "Legacy catch nodes are unsupported");
}

/** Interfaces group concrete owning payloads without allocating wrappers. */
TEST(Errors_Integration, typed_interface_patterns) {
  EXPECT_EQ(executeString(R"(
    /** Provides common failure behavior. */
    interface Failure { const method code() i32; }
    /** Supplies a concrete failure. */
    class A implements Failure {
      /** Creates the error. */
      init() {}
      /** Reports its code. */
      public const method code() i32 { return 21; }
    }
    /** Owns distinct variants carrying the same concrete error. */
    enum Outcome { Ok, First(A), Second(A) }
    /** Handles both error variants through one interface arm. */
    function code(result: ref Outcome) i32 {
      return match result { Outcome.Ok => 0, (e: const ref Failure) => e.code() };
    }
    /** Exercises each matching variant. */
    function main() i32 {
      var first = Outcome.First(A()); var second = Outcome.Second(A());
      return code(first) + code(second);
    }
  )"),
            42);
}

/** Borrowed interface variants keep their views inline across function returns.
 */
TEST(Errors_Integration, borrowed_interface_payload) {
  EXPECT_EQ(executeString(R"(
    /** Provides common failure behavior. */
    interface Failure { const method code() i32; }
    /** Supplies a concrete failure. */
    class A implements Failure {
      /** Creates the error. */
      init() {}
      /** Reports its code. */
      public const method code() i32 { return 42; }
    }
    /** Borrows an error owned by another scope. */
    enum Borrowed { Ok, Failed(const ref Failure) }
    /** Returns a view of caller-owned storage. */
    function borrow(error: const ref A) Borrowed { return Borrowed.Failed(error); }
    /** Uses the view while its owner remains alive. */
    function main() i32 {
      var error = A(); var result = borrow(error);
      return match result { Borrowed.Ok => 0, Borrowed.Failed(e) => e.code() };
    }
  )"),
            42);
}

/** Borrowed error results cannot refer to a destroyed local. */
TEST(Errors_Integration, rejects_escaping_borrowed_payload) {
  EXPECT_THROW(executeString(R"(
    /** Provides common failure behavior. */
    interface Failure { const method code() i32; }
    /** Supplies a concrete failure. */
    class A implements Failure {
      /** Creates the error. */
      init() {}
      /** Reports its code. */
      public const method code() i32 { return 42; }
    }
    /** Borrows an existing error. */
    enum Borrowed { Ok, Failed(const ref Failure) }
    /** Must reject a reference to a local error. */
    function invalid() Borrowed { var error = A(); return Borrowed.Failed(error); }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** Typed patterns can inspect a concrete error without wrapping it in an enum.
 */
TEST(Errors_Integration, direct_error_pattern) {
  EXPECT_EQ(executeString(R"(
    /** Provides shared error behavior. */
    interface Failure { const method code() i32; }
    /** Implements that behavior. */
    class Error implements Failure {
      /** Creates the error. */
      init() {}
      /** Returns the code. */
      public const method code() i32 { return 42; }
    }
    /** Borrows the concrete error through the interface. */
    function main() i32 {
      var error = Error();
      return match error { (e: const ref Failure) => e.code() };
    }
  )"),
            42);
}

/** Borrowed variants prevent moving their referents while the views remain
 * live. */
TEST(Errors_Integration, rejects_moving_borrowed_error) {
  EXPECT_THROW(executeString(R"(
    /** Provides shared error behavior. */
    interface Failure { const method code() i32; }
    /** Implements that behavior. */
    class Error implements Failure {
      /** Creates the error. */
      init() {}
      /** Returns the code. */
      public const method code() i32 { return 42; }
    }
    /** Holds an interface borrow. */
    enum Borrowed { Failed(const ref Failure) }
    /** Attempts to invalidate a borrowed payload. */
    function main() i32 {
      var error = Error(); var result = Borrowed.Failed(error);
      var moved = error;
      return match result { Borrowed.Failed(e) => e.code() };
    }
  )"),
               sun::support::SunError);
}

/** Mutable typed bindings cannot bypass an immutable discriminant. */
TEST(Errors_Integration, rejects_mutable_pattern_on_const_value) {
  EXPECT_THROW(executeString(R"(
    /** Stores a mutable field. */
    class Error {
      public var value: i32 = 0;
    }
    /** Owns a concrete error. */
    enum Failure { Error(Error) }
    /** Attempts to mutate an immutable enum payload. */
    function main() i32 {
      const result = Failure.Error(Error());
      match result { (e: ref Error) => { e.value = 42; } };
      return 0;
    }
  )"),
               sun::support::SunError);
}

/** Cloning retains the source forms needed by generic bodies and Moon exports.
 */
TEST(Errors_Integration, syntax_serialization) {
  const std::string source = R"(
    /** Exercises native handlers alongside ordinary typed enum matches. */
    function example() i32 throws {
      try { source(); } catch { throw; }
      return match value { (_: const ref IError) => 0, _ => 42 };
    }
  )";
  std::istringstream stream(source);
  sun::parsing::Parser parser(stream);
  auto ast = parser.parseString(source);
  auto cloned = ast->clone();
  int handlers = 0;
  int typed = 0;
  std::function<void(const sun::ast::ExprAST&)> visit =
      [&](const sun::ast::ExprAST& node) {
        if (dynamic_cast<const sun::ast::TryCatchAST*>(&node)) ++handlers;
        if (auto* match = dynamic_cast<const sun::ast::MatchExprAST*>(&node)) {
          for (const auto& arm : match->getArms()) {
            if (!arm.bindingType) continue;
            ++typed;
            EXPECT_TRUE(arm.bindingType->isReference());
            EXPECT_TRUE(arm.bindings.front().isWildcard);
          }
        }
        sun::ast::forEachChild(node, visit);
      };
  visit(*cloned);
  EXPECT_EQ(handlers, 1);
  EXPECT_EQ(typed, 1);
}

/** Borrowed error categories can be handled together through a parent
 * interface. */
TEST(Errors_Integration, borrowed_error_interfaces) {
  EXPECT_EQ(executeString(R"(
    /** Provides the shared view exercised by this interface-specific test. */
    interface ErrorView {
      /** Identifies a fixture error. */
      const method code() i32;
    }

    /** Marks failures for which another attempt can succeed. */
    interface IRetryableError extends ErrorView {
      const method delay() i32;
    }
    /** Supplies one concrete retryable failure. */
    class Retry implements IRetryableError {
      /** Creates an error. */
      init() {}
      /** Identifies the error. */
      public const method code() i32 { return 21; }
      /** Suggests a delay. */
      public const method delay() i32 { return 1; }
    }
    /** Borrows either category of error. */
    enum BorrowedResult {
      Ok,
      RetryableError(const ref IRetryableError),
      OtherError(const ref ErrorView)
    }
    /** Handles both categories through their shared interface. */
    function inspect(result: const ref BorrowedResult) i32 {
      return match result {
        BorrowedResult.Ok => 0,
        (e: const ref ErrorView) => e.code()
      };
    }
    /** Keeps both borrowed results inside their owner's lifetime. */
    function main() i32 {
      var error = Retry();
      var retry = BorrowedResult.RetryableError(error);
      var other = BorrowedResult.OtherError(error);
      return inspect(retry) + inspect(other);
    }
  )"),
            42);
}

/** Ordinary matches preserve the bounds of borrowed payloads returned by an
 * arm. */
TEST(Errors_Integration, rejects_match_borrow_escape) {
  EXPECT_THROW(executeString(R"(
    /** Keeps a borrow of caller-owned storage. */
    enum Borrowed { Value(const ref i32) }
    /** Must reject the returned view of local storage. */
    function invalid() Borrowed {
      var local = 42;
      return match true { true => Borrowed.Value(local), false => Borrowed.Value(local) };
    }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** Matching a borrowed result may return a view of caller-owned storage. */
TEST(Errors_Integration, match_forwards_borrowed_payload) {
  EXPECT_EQ(executeString(R"(
    /** Holds a borrow of the caller's value. */
    enum Borrowed { Value(const ref i32) }
    /** Chooses a view without shortening its owner's lifetime. */
    function choose(value: const ref i32, first: bool) Borrowed {
      return match first { true => Borrowed.Value(value), false => Borrowed.Value(value) };
    }
    /** Uses the returned borrow while its owner lives. */
    function main() i32 {
      var value = 42;
      return match choose(value, true) { Borrowed.Value(v) => v };
    }
  )"),
            42);
}

/** Nested-result lifetime tracking must preserve ordinary reference-call
 * checks. */
TEST(Errors_Integration, reference_call_cannot_escape_local) {
  EXPECT_THROW(executeString(R"(
    /** Forwards a borrow of the caller's storage. */
    function borrow(value: ref i32) ref i32 { return value; }
    /** Attempts to return a borrow into its own frame. */
    function invalid() ref i32 { var local = 42; return borrow(local); }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** The former typed throws clauses and invalid payloads remain rejected. */
TEST(Errors_Integration, removed_exception_syntax) {
  for (const std::string declaration :
       {"/** Old function. */ function f() void throws IError {}",
        "/** Old constructor. */ class C { /** Initializes C. */ init() throws "
        "IError {} }",
        "/** Old interface. */ interface I { /** Runs I. */ method f() void "
        "throws IError; }",
        "/** Old pointer. */ function f(cb: function () void throws IError) "
        "void {}",
        "/** Old lambda type. */ function f(cb: () => void throws IError) void "
        "{}"}) {
    SCOPED_TRACE(declaration);
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        executeString(declaration + " /** Supplies an entry point. */ function "
                                    "main() i32 { return 0; }"),
        "'throws' does not take an error type");
  }
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(
          "/** Rejects an old throw. */ function main() i32 { throw 1; }"),
      "throw requires a try block or a throws declaration");
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString("/** Rejects an old handler. */ function main() i32 { try "
                    "{} catch (e: ref IError) {} return 0; }"),
      "Unknown type");
}

/** Returning a result as its payload must be diagnosed before code generation. */
TEST(Errors_Integration, result_cannot_be_returned_as_its_payload) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Provides a compound success payload. */
    class Value { var number: i32 = 42; }
    /** Stores the success payload separately from its variant tag. */
    enum Outcome { Ok(Value), Failed }
    /** Incorrectly returns the tagged result instead of extracting its payload. */
    function invalid() Value { return Outcome.Ok(Value()); }
    /** Supplies an entry point for analysis. */
    function main() i32 { return 0; }
  )"), "Return value has type");
}

/** Consumed call and match results must not accumulate cleanup at later tries. */
TEST(Errors_Integration, consumed_temporaries_have_linear_cleanup_ir) {
  /** Counts unoptimized instructions while increasing independent propagations. */
  const auto instructionCount = [](int repetitions) {
    std::string source = R"(
      /** Gives a successful result an owning payload. */
      class Owner {
        /** Creates the owner. */
        init() {}
        /** Exercises transfer into a consuming function. */
        deinit() {}
      }
      /** Returns a result from an ordinary function. */
      function source() void throws { return; }
      /** Returns a result from a generic function. */
      function generic_source<T>() void throws { return; }
      /** Supplies a method returning an owned result. */
      class Factory {
        /** Creates the factory. */
        init() {}
        /** Returns a successful result. */
        method source() void throws { return; }
      }
      /** Returns an owner to move through a throwing call. */
      function make() Owner throws { return Owner(); }
      /** Takes responsibility for the successful payload. */
      function consume(owner: Owner) void {}
      /** Repeats transfers without retaining an owner between statements. */
      function run() void throws {
        var factory = Factory();
    )";
    for (int i = 0; i < repetitions; ++i) {
      source += "source(); generic_source<i32>(); "
                "factory.source(); consume(make());\n";
    }
    source += "return; }";
    auto driver = sun::driver::Driver::createForAOT(
        "cleanup_growth", "", false, false);
    driver->compileString(source);
    size_t count = 0;
    for (const auto& function : driver->getModule()) {
      for (const auto& block : function) {
        count += block.size();
      }
    }
    return count;
  };
  const auto small = instructionCount(8);
  const auto large = instructionCount(16);
  EXPECT_LT(large * 2, small * 5)
      << "Doubling independent throwing calls should not produce quadratic cleanup: "
      << small << " -> " << large;
}

/** Legacy propagation syntax is rejected rather than silently changing behavior. */
TEST(Errors_Integration, rejects_prefix_try) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::analyzeString(R"(
    /** Supplies an ordinary value. */
    function source() i32 { return 42; }
    /** Attempts to use the removed prefix syntax. */
    function caller() i32 throws { return try source(); }
  )"), "Prefix try was removed");
}

/** Removed result and discard builtins cannot conceal unhandled failures. */
TEST(Errors_Integration, rejects_removed_builtins) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::analyzeString(R"(
    /** Attempts to name the removed builtin wrapper. */
    function source() _Result<i32, bool> { return _Result.Ok(42); }
  )"), "Unknown generic type");
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Attempts to use the removed discard intrinsic. */
    function main() i32 { ignore_error(42); return 0; }
  )"), "Unknown");
}

/** Successful direct returns and failure propagation evaluate each call once. */
TEST(Errors_Integration, propagation_evaluates_once) {
  EXPECT_EQ(executeString(R"(
    var calls: i32 = 0;
    /** Carries a concrete failure code. */
    exception Failure { public var code: i32 = 7; }
    /** Counts evaluation before succeeding or failing. */
    function source(fail: bool) i32 throws {
      calls += 1;
      if (fail) { throw Failure(); }
      return 35;
    }
    /** Propagates the exception without a return wrapper. */
    function forward(fail: bool) i32 throws { return source(fail); }
    /** Combines successful and failed paths after checking evaluation counts. */
    function main() i32 throws {
      var value = forward(false);
      try { forward(true); } catch (error: Failure) {
        if (calls != 2) { return -1; }
        return value + error.code;
      }
      return -2;
    }
  )"), 42);
}

/** Throwing reference returns preserve the caller's array view without copying. */
TEST(Errors_Integration, borrowed_array_success) {
  EXPECT_EQ(executeString(R"(
    /** Forwards caller-owned storage through a throwing signature. */
    function borrow(values: ref array<i32>) ref array<i32> throws { return values; }
    /** Mutates through the returned view. */
    function update(values: ref array<i32>) void throws {
      var view: ref array<i32> = borrow(values);
      view[0] = 42;
    }
    /** Keeps storage live until the mutation is observed. */
    function main() i32 throws {
      var values: array<i32, 1> = [0];
      update(values);
      return values[0];
    }
  )"), 42);
}

/** A throws annotation cannot permit references into an unwound frame to escape. */
TEST(Errors_Integration, throwing_reference_cannot_escape_local) {
  EXPECT_THROW(executeString(R"(
    /** Returns its caller's reference. */
    function borrow(value: const ref i32) const ref i32 throws { return value; }
    /** Attempts to leak frame-owned storage. */
    function invalid() const ref i32 throws { var value = 42; return borrow(value); }
    /** Forces analysis without executing the invalid function. */
    function main() i32 { return 0; }
  )"), sun::support::SunError);
}

/** Earlier arguments are destroyed once if a later argument throws. */
TEST(Errors_Integration, pending_argument_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops: i32 = 0;
    /** Records ownership of a pending argument. */
    class Owner {
      /** Creates the resource. */
      init() {}
      /** Records release. */
      deinit() { drops += 1; }
    }
    /** Identifies failure without owning the pending resource. */
    exception Failure {}
    /** Produces an owning temporary as the first argument. */
    function make() Owner { return Owner(); }
    /** Fails during evaluation of the second argument. */
    function fail() i32 throws { throw Failure(); }
    /** Would consume both arguments if evaluation completed. */
    function consume(owner: Owner, number: i32) i32 { return number; }
    /** Checks cleanup of the abandoned call. */
    function main() i32 {
      try { consume(make(), fail()); } catch (error: Failure) { return drops; }
      return -1;
    }
  )"), 1);
}

/** Failed generic constructors destroy initialized fields but not incomplete objects. */
TEST(Errors_Integration, partial_generic_constructor_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops: i32 = 0;
    var completed: i32 = 0;
    /** Records each initialized field's release. */
    class Owner {
      /** Acquires a field resource. */
      init() {}
      /** Releases the resource. */
      deinit() { drops += 1; }
    }
    /** Identifies failure during initialization. */
    exception Failure {}
    /** Initializes fields on either side of a possible exception. */
    class Object<T> {
      var first: Owner;
      var second: Owner;
      public var value: T;
      /** Exercises partial and complete initialization. */
      init(value: T, fail: bool) throws {
        this.first = Owner();
        if (fail) { throw Failure(); }
        this.second = Owner();
        this.value = value;
      }
      /** Runs only for a fully initialized object. */
      deinit() { completed += 1; }
    }
    /** Leaves successful and failed objects in separate scopes. */
    function attempt(fail: bool) void throws { var value = Object<i32>(42, fail); }
    /** Verifies one release per initialized field and one complete destructor. */
    function main() i32 throws {
      try { attempt(true); } catch (error: Failure) {}
      attempt(false);
      return drops * 10 + completed;
    }
  )"), 31);
}

/** Native constructors still require complete initialization on successful paths. */
TEST(Errors_Integration, throwing_constructor_requires_complete_success) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Leaves a required field uninitialized. */
    class Incomplete {
      var value: i32;
      /** Incorrectly completes without initializing the field. */
      init() throws {}
    }
    /** Forces declaration analysis. */
    function main() i32 { return 0; }
  )"), "unassigned");
}

/** In-place construction propagates its exception before caller-owned storage is freed. */
TEST(Errors_Integration, in_place_initialization_throws) {
  EXPECT_EQ(executeString(R"(
    /** Carries the construction status. */
    exception Failure { public var code: i32 = 42; }
    /** Always fails during construction. */
    class Object {
      /** Rejects initialization. */
      init() throws { throw Failure(); }
    }
    /** Releases raw backing storage on both paths and returns the caught code. */
    function main() i32 {
      unsafe {
        var allocation = _malloc(_sizeof<Object>() + 1);
        var pointer = _bitcast<raw_ptr<Object>>(allocation);
        var code: i32 = 0;
        try { _init<Object>(pointer); } catch (error: Failure) { code = error.code; }
        _free(allocation);
        return code;
      };
    }
  )"), 42);
}
