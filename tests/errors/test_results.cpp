/** Verifies value-based errors, propagation, and borrowed interface patterns.
 */
#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>

#include <functional>
#include <sstream>

#include "ast/ast_children.h"
#include "driver/execution_utils.h"
#include "parsing/formatter.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"

using sun::driver::executeString;

/** Propagation unwraps success and returns an error without running later code.
 */
TEST(Errors_Results, propagation) {
  EXPECT_EQ(executeString(R"(
    /** Produces either branch of the builtin result. */
    function source(fail: bool) _Result<i32, i32> {
      if (fail) { return _Result.Err(7); }
      return _Result.Ok(35);
    }
    /** Uses the unwrapped value or propagates failure. */
    function caller(fail: bool) _Result<i32, i32> {
      var value = try source(fail);
      return _Result.Ok(value + 7);
    }
    /** Checks both outcomes. */
    function main() i32 {
      var good = match caller(false) { _Result.Ok(v) => v, _Result.Err(e) => 100 };
      var bad = match caller(true) { _Result.Ok(v) => 100, _Result.Err(e) => e };
      return good + bad;
    }
  )"),
            49);
}

/** Void successes have no payload and can be propagated as statements. */
TEST(Errors_Results, void_success) {
  EXPECT_EQ(executeString(R"(
    /** Returns an empty successful result. */
    function ready() _Result<void, i32> { return _Result.Ok; }
    /** Continues after propagating an empty result. */
    function answer() _Result<i32, i32> { try ready(); return _Result.Ok(42); }
    /** Extracts the answer. */
    function main() i32 {
      return match answer() { _Result.Ok(v) => v, _Result.Err(e) => e };
    }
  )"),
            42);
}

/** Unique wrapping preserves the concrete payload in the caller's error enum.
 */
TEST(Errors_Results, unique_error_wrapping) {
  EXPECT_EQ(executeString(R"(
    /** Groups failures from different sources. */
    enum Failure { Code(i32), Flag(bool) }
    /** Produces a concrete error. */
    function source() _Result<i32, i32> { return _Result.Err(42); }
    /** Propagates into the unique compatible variant. */
    function caller() _Result<i32, Failure> {
      var value = try source(); return _Result.Ok(value);
    }
    /** Reads the propagated payload. */
    function main() i32 {
      return match caller() {
        _Result.Ok(v) => 0,
        _Result.Err(e) => match e { Failure.Code(code) => code, Failure.Flag(_) => 0 }
      };
    }
  )"),
            42);
}

/** Ambiguous wrappers cannot silently select a category. */
TEST(Errors_Results, ambiguous_error_wrapping) {
  EXPECT_THROW(executeString(R"(
    /** Contains ambiguous categories for the same error type. */
    enum Failure { First(i32), Second(i32) }
    /** Produces a concrete error. */
    function source() _Result<i32, i32> { return _Result.Err(42); }
    /** Must reject ambiguous propagation. */
    function caller() _Result<i32, Failure> { return _Result.Ok(try source()); }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** Propagation cannot escape an ordinary scalar-returning function. */
TEST(Errors_Results, incompatible_return_type) {
  EXPECT_THROW(executeString(R"(
    /** Returns a result. */
    function source() _Result<i32, i32> { return _Result.Ok(42); }
    /** Cannot propagate errors through its scalar signature. */
    function main() i32 { return try source(); }
  )"),
               sun::support::SunError);
}

/** Resource-owning errors and other locals are destroyed exactly once. */
TEST(Errors_Results, propagation_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops: i32 = 0;
    /** Counts resource destruction. */
    class Resource {
      /** Creates a live resource. */
      init() {}
      /** Records release. */
      deinit() { drops = drops + 1; }
    }
    /** Returns an owned error. */
    function source() _Result<i32, Resource> { return _Result.Err(Resource()); }
    /** Releases its local when the error propagates. */
    function caller() _Result<i32, Resource> {
      var local = Resource();
      var value = try source();
      return _Result.Ok(value);
    }
    /** Handles and drops the returned error. */
    function run() void {
      match caller() { _Result.Ok(_) => {}, _Result.Err(_) => {} };
    }
    /** Reports the number of released resources. */
    function main() i32 { run(); return drops; }
  )"),
            2);
}

/** Interfaces group concrete owning payloads without allocating wrappers. */
TEST(Errors_Results, typed_interface_patterns) {
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
TEST(Errors_Results, borrowed_interface_payload) {
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
TEST(Errors_Results, rejects_escaping_borrowed_payload) {
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

/** Reference successes remain borrows after propagation. */
TEST(Errors_Results, reference_success) {
  EXPECT_EQ(executeString(R"(
    /** Returns a borrow of the caller's integer. */
    function source(value: ref i32) _Result<ref i32, bool> { return _Result.Ok(value); }
    /** Mutates through the successfully propagated reference. */
    function change(value: ref i32) _Result<void, bool> {
      var borrowed: ref i32 = try source(value);
      borrowed = 42;
      return _Result.Ok;
    }
    /** Verifies that the original storage changed. */
    function main() i32 {
      var value = 0;
      match change(value) { _Result.Ok => {}, _Result.Err(_) => {} };
      return value;
    }
  )"),
            42);
}

/** Propagation binds more tightly than arithmetic and evaluates the call once.
 */
TEST(Errors_Results, precedence_and_single_evaluation) {
  EXPECT_EQ(executeString(R"(
    var calls = 0;
    /** Counts evaluations. */
    function source() _Result<i32, bool> { calls += 1; return _Result.Ok(20); }
    /** Uses prefix propagation as an arithmetic operand. */
    function answer() _Result<i32, bool> { return _Result.Ok(try source() * 2 + 1); }
    /** Combines the value with the evaluation count. */
    function main() i32 {
      var value = match answer() { _Result.Ok(v) => v, _Result.Err(_) => 0 };
      return value + calls;
    }
  )"),
            42);
}

/** Typed patterns can inspect a concrete error without wrapping it in an enum.
 */
TEST(Errors_Results, direct_error_pattern) {
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

/** A typed wildcard retains its type filter without creating a binding. */
TEST(Errors_Results, typed_wildcard) {
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
    /** Exercises a typed wildcard and an unconditional fallback. */
    function main() i32 {
      var result: _Result<i32, Error> = _Result.Err(Error());
      return match result { (_: const ref Failure) => 42, _ => 0 };
    }
  )"),
            42);
}

/** Borrowed variants prevent moving their referents while the views remain
 * live. */
TEST(Errors_Results, rejects_moving_borrowed_error) {
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
TEST(Errors_Results, rejects_mutable_pattern_on_const_value) {
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

/** A later failing argument releases values already moved into earlier slots.
 */
TEST(Errors_Results, pending_argument_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops = 0;
    /** Counts ownership releases. */
    class Owner {
      /** Creates the owner. */
      init() {}
      /** Releases its resource. */
      deinit() { drops += 1; }
    }
    /** Fails before the outer call can start. */
    function fail() _Result<i32, bool> { return _Result.Err(true); }
    /** Would consume the first argument if reached. */
    function consume(owner: Owner, value: i32) i32 { return value; }
    /** Evaluates an owner before a propagating argument. */
    function run() _Result<i32, bool> { return _Result.Ok(consume(Owner(), try fail())); }
    /** Verifies the pending owner was released. */
    function main() i32 {
      match run() { _Result.Ok(_) => {}, _Result.Err(_) => {} };
      return drops;
    }
  )"),
            1);
}

/** Nesting a borrowed error in _Result cannot hide a dangling reference. */
TEST(Errors_Results, rejects_nested_borrow_escape) {
  EXPECT_THROW(executeString(R"(
    /** Holds a borrow of an integer. */
    enum Borrowed { Error(const ref i32) }
    /** Attempts to return a nested borrow of local storage. */
    function invalid() _Result<void, Borrowed> {
      var local = 42;
      var error = Borrowed.Error(local);
      return _Result.Err(error);
    }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** Formatting preserves prefix propagation and parameter-shaped patterns. */
TEST(Errors_Results, formatter_roundtrip) {
  const std::string source = R"(
    /** Propagates a result and handles a borrowed interface. */
    function example() _Result<i32, i32> {
      try prepare();
      var value = try source();
      return match value {
        (e: const ref IError) => _Result.Err(e.code()),
        _ => _Result.Ok(42)
      };
    }
  )";
  const auto formatted = sun::parsing::formatSource(source);
  EXPECT_NE(formatted.find("try prepare();"), std::string::npos);
  EXPECT_EQ(formatted.find("(try prepare())"), std::string::npos);
  EXPECT_NE(formatted.find("try source()"), std::string::npos);
  EXPECT_NE(formatted.find("(e: const ref IError)"), std::string::npos);
  EXPECT_EQ(sun::parsing::formatSource(formatted), formatted);
}

/** Cloning retains the source forms needed by generic bodies and Moon exports.
 */
TEST(Errors_Results, syntax_serialization) {
  const std::string source = R"(
    /** Exercises both new match representations. */
    function example() i32 {
      var value = try source();
      return match value { (_: const ref IError) => 0, _ => 42 };
    }
  )";
  std::istringstream stream(source);
  sun::parsing::Parser parser(stream);
  auto ast = parser.parseString(source);
  auto cloned = ast->clone();
  int propagation = 0;
  int typed = 0;
  std::function<void(const sun::ast::ExprAST&)> visit =
      [&](const sun::ast::ExprAST& node) {
        if (auto* match = dynamic_cast<const sun::ast::MatchExprAST*>(&node)) {
          if (match->isPropagation()) ++propagation;
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
  EXPECT_EQ(propagation, 1);
  EXPECT_EQ(typed, 1);
}

/** Allocation-free result transport emits no exception or allocation calls. */
TEST(Errors_Results, transport_ir) {
  auto driver =
      sun::driver::Driver::createForAOT("result_transport", "", false, false);
  driver->compileString(R"(
    /** Returns a concrete failure. */
    function fail() _Result<i32, i32> { return _Result.Err(42); }
    /** Propagates the returned failure. */
    function run() _Result<i32, i32> { return _Result.Ok(try fail()); }
    /** Handles the failure without allocating. */
    function main() i32 {
      return match run() { _Result.Ok(v) => v, _Result.Err(e) => e };
    }
  )");
  for (const auto& function : driver->getModule()) {
    for (const auto& block : function) {
      for (const auto& instruction : block) {
        EXPECT_FALSE(llvm::isa<llvm::InvokeInst>(instruction));
        EXPECT_FALSE(llvm::isa<llvm::LandingPadInst>(instruction));
        auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call || !call->getCalledFunction()) continue;
        const auto name = call->getCalledFunction()->getName();
        EXPECT_FALSE(name.starts_with("__cxa_"));
        EXPECT_NE(name, "malloc");
        EXPECT_NE(name, "calloc");
        EXPECT_NE(name, "realloc");
      }
    }
  }
}

/** Propagation carries the lifetime bounds of success values containing
 * borrows. */
TEST(Errors_Results, rejects_propagated_borrow_escape) {
  EXPECT_THROW(executeString(R"(
    /** Keeps a borrow of caller-owned storage. */
    enum Borrowed { Value(const ref i32) }
    /** Wraps the borrow in a success. */
    function source(value: const ref i32) _Result<Borrowed, bool> {
      return _Result.Ok(Borrowed.Value(value));
    }
    /** Must reject returning a view of local storage. */
    function invalid() _Result<Borrowed, bool> {
      var local = 42;
      var borrowed = try source(local);
      return _Result.Ok(borrowed);
    }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** _Result expressions cannot be silently discarded. */
TEST(Errors_Results, rejects_discarded_result) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Produces a result requiring handling. */
    function source() _Result<i32, bool> { return _Result.Err(true); }
    /** Discards the failure. */
    function main() i32 { source(); return 0; }
  )"),
                                "Error result must be handled");
}

/** Storing a result in an unused local is still an unhandled result. */
TEST(Errors_Results, rejects_unused_result_local) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Produces a result requiring handling. */
    function source() _Result<i32, bool> { return _Result.Err(true); }
    /** Leaves the result unhandled. */
    function main() i32 { var unused = source(); return 0; }
  )"),
                                "Unused error result 'unused'");
}

/** Explicit discards release owned success or error payloads exactly once. */
TEST(Errors_Results, explicit_discard_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops = 0;
    /** Counts destruction of a result payload. */
    class Owner {
      /** Creates an owner. */
      init() {}
      /** Records destruction. */
      deinit() { drops += 1; }
    }
    /** Produces an owned failure. */
    function source() _Result<void, Owner> { return _Result.Err(Owner()); }
    /** Explicitly discards a temporary and a stored result. */
    function main() i32 {
      ignore_error(source());
      var result = source();
      ignore_error(result);
      return drops;
    }
  )"),
            2);
}

/** Borrowed error categories can be handled together through a parent
 * interface. */
TEST(Errors_Results, borrowed_error_interfaces) {
  EXPECT_EQ(executeString(R"(
    /** Marks failures for which another attempt can succeed. */
    interface IRetryableError extends IError {
      const method delay() i32;
    }
    /** Supplies one concrete retryable failure. */
    class Retry implements IRetryableError {
      /** Creates an error. */
      init() {}
      /** Identifies the error. */
      public const method code() i32 { return 21; }
      /** Describes the error. */
      public const method message() static_ptr<u8> { return "retry"; }
      /** Suggests a delay. */
      public const method delay() i32 { return 1; }
    }
    /** Borrows either category of error. */
    enum BorrowedResult {
      Ok,
      RetryableError(const ref IRetryableError),
      OtherError(const ref IError)
    }
    /** Handles both categories through their shared interface. */
    function inspect(result: const ref BorrowedResult) i32 {
      return match result {
        BorrowedResult.Ok => 0,
        (e: const ref IError) => e.code()
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

/** _Result can explicitly borrow an interface while owned concrete errors stay
 * the default. */
TEST(Errors_Results, result_borrowed_interface_error) {
  EXPECT_EQ(executeString(R"(
    /** Provides a concrete failure to borrow. */
    class Error implements IError {
      /** Creates the error. */
      init() {}
      /** Identifies it. */
      public const method code() i32 { return 42; }
      /** Describes it. */
      public const method message() static_ptr<u8> { return "failed"; }
    }
    /** Borrows the caller's error explicitly. */
    function source(error: const ref Error) _Result<void, const ref IError> {
      return _Result.Err(error);
    }
    /** Propagates the borrowed interface without making an owned copy. */
    function forward(error: const ref Error) _Result<void, const ref IError> {
      try source(error);
      return _Result.Ok;
    }
    /** Handles the borrow while the concrete error is alive. */
    function main() i32 {
      var error = Error();
      return match forward(error) { _Result.Ok => 0, (e: const ref IError) => e.code() };
    }
  )"),
            42);
}

/** Ordinary matches preserve the bounds of borrowed payloads returned by an
 * arm. */
TEST(Errors_Results, rejects_match_borrow_escape) {
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
TEST(Errors_Results, match_forwards_borrowed_payload) {
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

/** Array successes keep their complete borrowed view through propagation. */
TEST(Errors_Results, array_reference_success) {
  EXPECT_EQ(executeString(R"(
/** Borrows a caller-owned array. */
function source(value: ref array<i32>) _Result<ref array<i32>, bool> { return _Result.Ok(value); }
/** Mutates through a propagated array view. */
function change(value: ref array<i32>) _Result<void, bool> {
  var view: ref array<i32> = try source(value);
  view[0] = 42;
  return _Result.Ok;
}
/** Checks the mutation reaches the caller's storage. */
function main() i32 {
  var values: array<i32, 1> = [0];
  match change(values) { _Result.Ok => {}, _Result.Err(_) => {} };
  return values[0];
}

)"),
            42);
}

/** Fallible construction returns an owned object on success and a concrete
 * error on failure. */
TEST(Errors_Results, fallible_constructor) {
  EXPECT_EQ(executeString(R"(
    /** Validates the stored value before construction completes. */
    class Value {
      public var value: i32;
      /** Fails before initializing its field for negative values. */
      init(value: i32) _Result<void, i32> {
        if (value < 0) { return _Result.Err(7); }
        this.value = value;
      }
    }
    /** Propagates construction errors and reads the initialized object. */
    function read(value: i32) _Result<i32, i32> {
      var object = try Value(value);
      return _Result.Ok(object.value);
    }
    /** Checks both construction paths. */
    function main() i32 {
      var success = match read(35) { _Result.Ok(v) => v, _Result.Err(_) => 0 };
      var failure = match read(-1) { _Result.Ok(_) => 0, _Result.Err(e) => e };
      return success + failure;
    }
  )"),
            42);
}

/** A failed constructor drops initialized fields, including writes inside
 * branches. */
TEST(Errors_Results, fallible_constructor_partial_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops = 0;
    var incomplete_drops = 0;
    /** Counts releases independently of zero-initialized storage. */
    class Owner {
      /** Creates an owner. */
      init() {}
      /** Records its release. */
      deinit() { drops += 1; }
    }
    /** Owns fields that may be initialized before construction fails. */
    class Partial {
      var first: Owner;
      var second: Owner;
      /** Initializes one field before an early error return. */
      init(fail: bool) _Result<void, bool> {
        if (fail) { this.first = Owner(); return _Result.Err(true); }
        this.first = Owner();
        this.second = Owner();
        return;
      }
      /** Runs only for a complete object. */
      deinit() { incomplete_drops += 1; }
    }
    /** Discards both results, including the successful complete object. */
    function main() i32 {
      ignore_error(Partial(true));
      ignore_error(Partial(false));
      return drops * 10 + incomplete_drops;
    }
  )"),
            31);
}

/** Generic classes preserve their construction result after specialization. */
TEST(Errors_Results, generic_fallible_constructor) {
  EXPECT_EQ(executeString(R"(
    /** Owns a value of the specialized type. */
    class Box<T> {
      public var value: T;
      /** Completes construction after taking ownership of the argument. */
      init(value: T) _Result<void, bool> { this.value = value; }
    }
    /** Handles the construction result. */
    function main() i32 {
      return match Box<i32>(42) { _Result.Ok(box) => box.value, _Result.Err(_) => 0 };
    }
  )"),
            42);
}

/** Fallible construction still initializes every field on successful paths. */
TEST(Errors_Results, fallible_constructor_requires_complete_success) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Demonstrates an incomplete successful construction. */
    class Incomplete {
      var missing: i32;
      /** Incorrectly reports success without assigning its field. */
      init() _Result<void, bool> {}
    }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
                                "unassigned");
}

/** Prefix propagation in init releases defaults and preserves the owned error.
 */
TEST(Errors_Results, fallible_constructor_propagation_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops = 0;
    /** Counts resources owned by fields and errors. */
    class Owner {
      /** Creates a resource. */
      init() {}
      /** Releases it. */
      deinit() { drops += 1; }
    }
    /** Produces an owning error instead of another field value. */
    function fail() _Result<Owner, Owner> { return _Result.Err(Owner()); }
    /** Acquires a default before a later field initialization fails. */
    class Partial {
      var first: Owner = Owner();
      var second: Owner;
      /** Propagates the failure before initializing the second field. */
      init() _Result<void, Owner> { this.second = try fail(); }
    }
    /** Releases the error after the constructor has released its default. */
    function main() i32 { ignore_error(Partial()); return drops; }
  )"),
            2);
}

/** In-place initialization returns the constructor's error for explicit
 * handling. */
TEST(Errors_Results, fallible_in_place_initialization) {
  EXPECT_EQ(executeString(R"(
    /** Always fails before any instance becomes live. */
    class Failed {
      /** Reports a concrete construction error. */
      init() _Result<void, i32> { return _Result.Err(42); }
    }
    /** Handles the result before releasing the raw allocation. */
    function main() i32 {
      unsafe {
        var allocation = _malloc(_sizeof<Failed>() + 1);
        var pointer = _bitcast<raw_ptr<Failed>>(allocation);
        var result = _init<Failed>(pointer);
        var code = match result { _Result.Ok => 0, _Result.Err(e) => e };
        _free(allocation);
        return code;
      };
    }
  )"),
            42);
}

/** Constructor helpers make initialized fields eligible for failure cleanup. */
TEST(Errors_Results, fallible_constructor_helper_cleanup) {
  EXPECT_EQ(executeString(R"(
    var drops = 0;
    /** Releases only an initialized resource. */
    class Owner {
      var active: bool;
      /** Acquires a resource. */
      init() { this.active = true; }
      /** Records release of a live resource. */
      deinit() { if (this.active) { drops += 1; } }
    }
    /** Initializes its field through a helper before failing. */
    class Partial {
      var field: Owner;
      /** Fails after the helper returns. */
      init() _Result<void, bool> { this.prepare(); return _Result.Err(true); }
      /** Acquires the field's resource. */
      method prepare() void { this.field = Owner(); }
    }
    /** Checks that the initialized field was released. */
    function main() i32 { ignore_error(Partial()); return drops; }
  )"),
            1);
}

/** Nested-result lifetime tracking must preserve ordinary reference-call
 * checks. */
TEST(Errors_Results, reference_call_cannot_escape_local) {
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

/** Checked division and remainder return concrete errors for every integer
 * width. */
TEST(Errors_Results, checked_arithmetic) {
  for (bool optimize : {false, true}) {
    for (const std::string type :
         {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"}) {
      for (const std::string operation : {"checked_div", "checked_rem"}) {
        SCOPED_TRACE(type + operation);
        std::string source =
            "/** Runs a checked operation. */\nfunction calculate(a: " + type +
            ", b: " + type + ") _Result<" + type +
            ", ArithmeticError> { return " + operation +
            "(a, b); }\n"
            "/** Handles both result variants. */\nfunction main() i32 {\n"
            "var success = match calculate(7, 2) { _Result.Ok(v) => "
            "_convert<i32>(v), _Result.Err(e) => -100 };\n"
            "var error = match calculate(7, 0) { _Result.Ok(v) => -100, "
            "_Result.Err(e) => e.code() };\n"
            "return success + error; }";
        auto driver = sun::driver::Driver::createForJIT("checked_arithmetic",
                                                        false, optimize);
        EXPECT_EQ(driver->executeString(source),
                  operation == "checked_div" ? 7 : 5);
      }
    }
  }
}

/** Signed overflow is an error for division and remainder, including optimized
 * code. */
TEST(Errors_Results, checked_arithmetic_signed_overflow) {
  for (bool optimize : {false, true}) {
    for (const std::string type : {"i8", "i16", "i32", "i64"}) {
      int width = std::stoi(type.substr(1));
      std::string minimum = width == 64
                                ? "-9223372036854775807 - 1"
                                : std::to_string(-(int64_t{1} << (width - 1)));
      for (const std::string operation : {"checked_div", "checked_rem"}) {
        auto driver = sun::driver::Driver::createForJIT("checked_overflow",
                                                        false, optimize);
        EXPECT_EQ(driver->executeString(
                      "/** Checks overflow without creating an invalid LLVM "
                      "operation. */\n"
                      "function main() i32 { var a: " +
                      type + " = " + minimum + "; var b: " + type +
                      " = -1;\n"
                      "return match " +
                      operation +
                      "(a, b) { _Result.Ok(v) => -1, _Result.Err(e) => "
                      "e.code() }; }"),
                  5);
      }
    }
  }
}

/** Checked arithmetic evaluates operands once and participates in propagation.
 */
TEST(Errors_Results, checked_arithmetic_propagation) {
  EXPECT_EQ(executeString(R"(
    var calls = 0;
    /** Records evaluation of an operand. */
    function operand(value: i32) i32 { calls += 1; return value; }
    /** Stops before the success return when division fails. */
    function compute() _Result<i32, ArithmeticError> {
      var value = try checked_div(operand(42), operand(0));
      return _Result.Ok(value);
    }
    /** Observes both the failure code and evaluation count. */
    function main() i32 {
      var code = match compute() { _Result.Ok(v) => -1, _Result.Err(e) => e.code() };
      return code * 10 + calls;
    }
  )"),
            42);
}

/** Different result specializations cannot be reinterpreted at a return
 * boundary. */
TEST(Errors_Results, incompatible_result_return) {
  EXPECT_THROW(executeString(R"(
    /** Attempts to return a result with a different payload layout. */
    function invalid() _Result<i64, bool> {
      var small: _Result<i32, bool> = _Result.Ok(42);
      return small;
    }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** An owned message survives the result and error that supplied it. */
TEST(Errors_Results, message_outlives_error) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Returns a message while dropping the result's concrete error. */
    function describe() String {
      var result: _Result<void, Error> = _Result.Err(Error(42, "a diagnostic longer than inline storage"));
      return match result {
        _Result.Ok => String("unexpected success"),
        _Result.Err(error) => error.message()
      };
    }
    /** Reads the independently owned message after the error was dropped. */
    function main() i32 {
      var message = describe();
      if (message.equals_literal("a diagnostic longer than inline storage")) { return 42; }
      return -1;
    }
  )"),
            42);
}

/** A module qualifier contributes no receiver storage; actual reference
 * arguments still do. */
TEST(Errors_Results, module_qualified_borrow_origins) {
  const std::string declarations = R"(
    /** Supplies a borrowed result. */
    module views {
      /** Returns a borrow of its argument. */
      public function borrow(value: const ref i32) _Result<const ref i32, bool> {
        return _Result.Ok(value);
      }
    }
  )";
  EXPECT_EQ(executeString(declarations + R"(
    /** Forwards the caller's storage. */
    function forward(value: const ref i32) _Result<const ref i32, bool> {
      return views.borrow(value);
    }
    /** Keeps the owner alive until the reference is read. */
    function main() i32 {
      var value = 42;
      return match forward(value) { _Result.Ok(v) => v, _Result.Err(_) => -1 };
    }
  )"),
            42);
  EXPECT_THROW(executeString(declarations + R"(
    /** Attempts to leak frame-owned storage through a module-qualified call. */
    function invalid() _Result<const ref i32, bool> {
      var value = 42;
      return views.borrow(value);
    }
    /** Supplies an entry point. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** Checks first variant with enum-order propagation. */
TEST(Errors_Results, enum_order_first_variant) {
  EXPECT_EQ(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Read<T> { Value(T), Missing(i32), Invalid(bool) }
/** Defines the fixture value used by propagation. */
enum Work { Done(i32), Bad(bool), Absent(i32) }
/** Exercises propagation and reports its observable result. */
function read(n:i32) Read<i32> { if (n==1) { return Read.Missing(7); } if (n==2) { return Read.Invalid(true); } return Read.Value(42); }
/** Exercises propagation and reports its observable result. */
function work(n:i32) Work { var value=try read(n); return Work.Done(value); }
/** Exercises propagation and reports its observable result. */
function code(n:i32) i32 { return match work(n) { Work.Done(v)=>v, Work.Bad(v)=>2, Work.Absent(v)=>v }; }
/** Exercises propagation and reports its observable result. */
function main() i32 { return code(0)+code(1)+code(2)-51; }
  )"),
            0);
}

/** Checks unit with enum-order propagation. */
TEST(Errors_Results, enum_order_unit) {
  EXPECT_EQ(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Ready<T> { Complete(T), Failed(i32) }
/** Exercises propagation and reports its observable result. */
function ready() Ready<void> { return Ready.Complete; }
/** Exercises propagation and reports its observable result. */
function work() Ready<i32> { try ready(); return Ready.Complete(42); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return match work() { Ready.Complete(v)=>v-42, Ready.Failed(e)=>1 }; }
  )"),
            0);
}

/** Checks same enum with enum-order propagation. */
TEST(Errors_Results, enum_order_same_enum) {
  EXPECT_EQ(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Status { Success(i32), First(i32), Second(i32) }
/** Exercises propagation and reports its observable result. */
function source() Status { return Status.Second(42); }
/** Exercises propagation and reports its observable result. */
function forward() Status { var n=try source(); return Status.Success(n); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return match forward() { Status.Success(_)=>1, Status.First(_)=>2, Status.Second(v)=>v-42 }; }
  )"),
            0);
}

/** Checks reference with enum-order propagation. */
TEST(Errors_Results, enum_order_reference) {
  EXPECT_EQ(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Borrowed<T> { Present(T), Missing(bool) }
/** Exercises propagation and reports its observable result. */
function borrow(v:ref i32) Borrowed<ref i32> { return Borrowed.Present(v); }
/** Exercises propagation and reports its observable result. */
function forward(v:ref i32) Borrowed<ref i32> { var r=try borrow(v); return Borrowed.Present(r); }
/** Exercises propagation and reports its observable result. */
function main() i32 { var v=42; return match forward(v) { Borrowed.Present(r)=>r-42, Borrowed.Missing(_)=>1 }; }
  )"),
            0);
}

/** Checks cleanup with enum-order propagation. */
TEST(Errors_Results, enum_order_cleanup) {
  EXPECT_EQ(executeString(R"(
var drops:i32=0;
/** Defines the fixture value used by propagation. */
class Owner { /** Creates a tracked resource. */ init() {} /** Records resource cleanup. */ deinit() { drops=drops+1; } }
/** Defines the fixture value used by propagation. */
enum Source { Value(i32), Failed(Owner) }
/** Defines the fixture value used by propagation. */
enum Target { Done(i32), Broken(Owner) }
/** Exercises propagation and reports its observable result. */
function fail() Source { return Source.Failed(Owner()); }
/** Exercises propagation and reports its observable result. */
function work() Target { var local=Owner(); var v=try fail(); return Target.Done(v); }
/** Exercises propagation and reports its observable result. */
function run() void { match work() { Target.Done(_)=>{}, Target.Broken(_)=>{} }; }
/** Exercises propagation and reports its observable result. */
function main() i32 { run(); return drops-2; }
  )"),
            0);
}

/** Checks missing with enum-order propagation. */
TEST(Errors_Results, enum_order_missing) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Source { Value(i32), Failed(bool) }
/** Defines the fixture value used by propagation. */
enum Target { Done(bool), Other(i32) }
/** Exercises propagation and reports its observable result. */
function source() Source { return Source.Value(42); }
/** Exercises propagation and reports its observable result. */
function work() Target { var n=try source(); return Target.Other(n); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return 0; }
  )"),
                                "Cannot propagate error variant");
}

/** Checks ambiguous with enum-order propagation. */
TEST(Errors_Results, enum_order_ambiguous) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Source { Value(i32), Failed(bool) }
/** Defines the fixture value used by propagation. */
enum Target { Done(i32), First(bool), Second(bool) }
/** Exercises propagation and reports its observable result. */
function source() Source { return Source.Value(42); }
/** Exercises propagation and reports its observable result. */
function work() Target { var n=try source(); return Target.Done(n); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return 0; }
  )"),
                                "Ambiguous error propagation");
}

/** Checks multiple success payloads with enum-order propagation. */
TEST(Errors_Results, enum_order_multiple_success_payloads) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Source { Pair(i32,i32), Failed(bool) }
/** Exercises propagation and reports its observable result. */
function source() Source { return Source.Pair(1,2); }
/** Exercises propagation and reports its observable result. */
function work() Source { var n=try source(); return Source.Failed(false); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return 0; }
  )"),
                                "zero or one success payload");
}

/** Checks unit failure with enum-order propagation. */
TEST(Errors_Results, enum_order_unit_failure) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Source { Value(i32), Failed }
/** Exercises propagation and reports its observable result. */
function source() Source { return Source.Value(42); }
/** Exercises propagation and reports its observable result. */
function work() Source { var n=try source(); return Source.Value(n); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return 0; }
  )"),
                                "each failure variant");
}

/** Checks borrow escape with enum-order propagation. */
TEST(Errors_Results, enum_order_borrow_escape) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Borrowed<T> { Present(T), Missing(bool) }
/** Exercises propagation and reports its observable result. */
function borrow(v:ref i32) Borrowed<ref i32> { return Borrowed.Present(v); }
/** Exercises propagation and reports its observable result. */
function bad() Borrowed<ref i32> { var local=42; var r=try borrow(local); return Borrowed.Present(r); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return 0; }
  )"),
                                "cannot return a value");
}

/** Checks legacy wrapper with enum-order propagation. */
TEST(Errors_Results, enum_order_legacy_wrapper) {
  EXPECT_EQ(executeString(R"(
/** Defines the fixture value used by propagation. */
enum Error { Code(i32), Flag(bool) }
/** Exercises propagation and reports its observable result. */
function source() _Result<i32,i32> { return _Result.Err(42); }
/** Exercises propagation and reports its observable result. */
function work() _Result<i32,Error> { var n=try source(); return _Result.Ok(n); }
/** Exercises propagation and reports its observable result. */
function main() i32 { return match work() { _Result.Ok(_)=>1, _Result.Err(e)=>match e { Error.Code(v)=>v-42, Error.Flag(_)=>2 } }; }
  )"),
            0);
}

/** Checks library results, builtin coexistence, propagation, and discard. */
TEST(Errors_Results, stdlib_result_and_access_result) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
using std;
/** Returns a precise vector error through the broader library result. */
function access(index:i64) Result<i32> {
  var alloc=make_heap_allocator();
  var values=Vec<i32>(alloc,2);
  values.push(42);
  return Result.Ok(try values.get(index));
}
/** Reads success and bounds errors while grouping other categories by interface. */
function code(index:i64) i32 {
  return match access(index) {
    Result.Ok(value)=>value,
    Result.OutOfBounds(error)=>error.code(),
    (_:const ref IError)=>-100
  };
}
/** Checks library results, the builtin, and explicit discard in one program. */
function main() i32 {
  var builtin:_Result<i32,bool>=_Result.Ok(1);
  var alloc=make_heap_allocator();
  var values=Vec<i32>(alloc,2);
  ignore_error(values.get(0));
  return code(0)+code(1)+match builtin { _Result.Ok(v)=>v, _Result.Err(_)=>0 }-46;
}

  )"),
            0);
}

/** Library result failures cannot be silently discarded. */
TEST(Errors_Results, rejects_discarded_library_result) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Attempts to discard a checked vector access. */
    function main() i32 {
      var alloc = make_heap_allocator();
      var values = Vec<i32>(alloc, 2);
      values.get(0);
      return 0;
    }
  )"),
                                "Error result must be handled");
}
