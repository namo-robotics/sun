/** Exercises throwing entrypoints and their process boundary behavior. */
#include <gtest/gtest.h>

#include "driver/driver.h"
#include "driver/execution_utils.h"

using sun::driver::executeString;
using sun::driver::executeStringWithStdlib;

/** Allocation failures may propagate directly from the program entrypoint. */
TEST(EndToEnd_ExceptionMain, propagates_from_allocating_code) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    /** Builds and reads an owned container. */
    function main() i32 throws {
      var values =  Vec<i32>(global_allocator, 1);
       values.push(42);
      return values[0];
    }
  )"), 42);
}

/** Throwing JIT entrypoints preserve wide integer return values. */
TEST(EndToEnd_ExceptionMain, preserves_i64) {
  EXPECT_EQ(executeString(R"(
    /** Returns a value wider than a process exit code. */
    function main() i64 throws { return 4294967296i64; }
  )"), 4294967296LL);
}

/** JIT entrypoints preserve floating-point success payloads. */
TEST(EndToEnd_ExceptionMain, preserves_f64) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns a fractional result. */
    function main() f64 throws { return 1.5; }
  )"), 1.5);
}

/** Empty success remains a void value during JIT execution. */
TEST(EndToEnd_ExceptionMain, preserves_void) {
  EXPECT_TRUE(sun::driver::isVoid(executeStringWithStdlib(R"(
    /** Completes without a scalar result. */
    function main() void throws { return; }
  )")));
}

/** String success uses the JIT's existing pointer-and-length return convention. */
TEST(EndToEnd_ExceptionMain, preserves_static_string) {
  const auto value = executeString(R"(
    /** Returns literal text. */
    function main() static_ptr<u8> throws { return "hello"; }
  )");
  EXPECT_EQ(std::get<std::string>(value), "hello");
}

/** The adapter forwards the original command-line arguments unchanged. */
TEST(EndToEnd_ExceptionMain, forwards_arguments) {
  char name[] = "test";
  char* argv[] = {name, nullptr};
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns the argument count through the exception boundary. */
    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) i32 throws {
      return argc;
    }
  )", 1, argv), 1);
}

/** An uncaught bounded exception produces a diagnostic and a failing exit code. */
TEST(EndToEnd_ExceptionMain, uncaught_exception_exits_one) {
  EXPECT_EXIT(executeString(R"(
    /** Carries the failure code without owning storage. */
    exception Failure { var code: i32 = 7; }
    /** Raises a failure at the process boundary. */
    function main() i32 throws { throw Failure(); }
  )"), testing::ExitedWithCode(1), "Unhandled main exception");
}

/** Stack resources are released before the entrypoint handler exits. */
TEST(EndToEnd_ExceptionMain, uncaught_exception_drops_locals) {
  EXPECT_EXIT(executeStringWithStdlib(R"(
    using std;
    /** Makes stack cleanup observable. */
    class Resource {
      /** Acquires a fixture resource. */
      init() {}
      /** Reports resource release without allocating. */
      deinit() { std.io.eprintln("dropped"); }
    }
    /** Identifies a bounded failure. */
    exception Failure {}
    /** Unwinds a live resource. */
    function main() i32 throws {
      var resource = Resource();
      throw Failure();
    }
  )"), testing::ExitedWithCode(1), "dropped\nUnhandled main exception");
}

/** Boundary reporting does not invoke an exception's allocating message method. */
TEST(EndToEnd_ExceptionMain, boundary_reporting_needs_no_message_allocation) {
  EXPECT_EXIT(executeStringWithStdlib(R"(
    using std;
    /** Provides a diagnostic method that must not be called by the boundary. */
    exception Failure implements IError {
      /** Identifies the failure. */
      public const method code() i32 { return 7; }
      /** Would fail if the process boundary tried to allocate a diagnostic. */
      public const method message() String throws { throw AllocationError(64); }
    }
    /** Raises the bounded exception. */
    function main() void throws { throw Failure(); }
  )"), testing::ExitedWithCode(1), "Unhandled main exception");
}

/** Owned class payloads cannot cross the native JIT entrypoint ABI. */
TEST(EndToEnd_ExceptionMain, rejects_unsupported_success) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** An owned value with no host return representation. */
    class Value { var number: i32 = 42; }
    /** Returns an owned class. */
    function main() Value throws { return Value(); }
  )"), "Unsupported return type for 'main': 'Value'");
}

/** Ordinary entrypoints also reject values the host cannot safely receive. */
TEST(EndToEnd_ExceptionMain, rejects_unsupported_plain_return) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Owns a value with no host return representation. */
    class Value { var number: i32 = 42; }
    /** Returns an unsupported owned value directly. */
    function main() Value { return Value(); }
  )"), "Unsupported return type for 'main': 'Value'");
}

/** Native executable entrypoints cannot return a wide success payload. */
TEST(EndToEnd_ExceptionMain, aot_rejects_i64_success) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForAOT("exception_main");
  driver->setExecutable(true);
  EXPECT_SUN_ERROR_WITH_MESSAGE(driver->compileString(R"(
    /** Returns a value unsuitable for a native exit code. */
    function main() i64 throws { return 42i64; }
  )"), "Unsupported return type for 'main': 'i64'");
}

/** Boolean payloads retain their native JIT representation. */
TEST(EndToEnd_ExceptionMain, preserves_bool) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns a boolean success. */
    function main() bool throws { return true; }
  )"), true);
}

/** Character payloads do not require an implicit integer conversion. */
TEST(EndToEnd_ExceptionMain, preserves_char) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns a character success. */
    function main() char throws { return 'A'; }
  )"), 65);
}

/** Recursive calls retain the source return type and exception contract. */
TEST(EndToEnd_ExceptionMain, recursive_calls_keep_return_type) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    var calls: i32 = 0;
    /** Calls the original function once before returning a scalar. */
    function main() i32 throws {
      calls += 1;
      if (calls == 1) { return  main(); }
      return 42;
    }
  )"), 42);
}

/** An entrypoint cannot expose multiple success values through one return slot. */
TEST(EndToEnd_ExceptionMain, rejects_multiple_success_payloads) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Carries an unsupported pair on success. */
    enum Outcome { Ready(i32, i32), Failed(i32) }
    /** Returns both values. */
    function main() Outcome { return Outcome.Ready(1, 2); }
  )"), "Unsupported return type for 'main'");
}

/** Payload enums have no native entrypoint representation. */
TEST(EndToEnd_ExceptionMain, rejects_empty_failure_variant) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Has no error payload on failure. */
    enum Outcome { Ready(i32), Failed }
    /** Returns a supported scalar in an invalid result shape. */
    function main() Outcome { return Outcome.Ready(42); }
  )"), "Unsupported return type for 'main'");
}

/** A void throwing main receives the native executable's exit status adapter. */
TEST(EndToEnd_ExceptionMain, aot_void_success_has_native_status) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForAOT("exception_main_void");
  driver->setExecutable(true);
  driver->compileString(R"(
    /** Completes successfully. */
    function main() void throws { return; }
  )");
  auto* main = driver->getModule().getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(main->getReturnType()->isIntegerTy(32));
}
