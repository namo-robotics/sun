/** Exercises result-enum entrypoints and their process boundary behavior. */
#include <gtest/gtest.h>

#include "driver/driver.h"
#include "driver/execution_utils.h"

using sun::driver::executeString;
using sun::driver::executeStringWithStdlib;

/** Allocation failures may propagate directly from the program entrypoint. */
TEST(EndToEnd_ResultMain, propagates_from_allocating_code) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    /** Builds and reads an owned container. */
    function main() Result<i32> {
      var values = try Vec<i32>(global_allocator, 1);
      try values.push(42);
      return Result.Ok(values[0]);
    }
  )"), 42);
}

/** Success uses declaration order rather than a hard-coded variant name. */
TEST(EndToEnd_ResultMain, custom_success_variant_preserves_i64) {
  EXPECT_EQ(executeString(R"(
    /** Describes a scalar computation without a standard-library dependency. */
    enum Outcome { Ready(i64), Failed(i32) }
    /** Returns a value wider than a process exit code. */
    function main() Outcome { return Outcome.Ready(4294967296i64); }
  )"), 4294967296LL);
}

/** JIT entrypoints preserve floating-point success payloads. */
TEST(EndToEnd_ResultMain, preserves_f64) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns a fractional result. */
    function main() std.Result<f64> { return std.Result.Ok(1.5); }
  )"), 1.5);
}

/** Empty success remains a void value during JIT execution. */
TEST(EndToEnd_ResultMain, preserves_void) {
  EXPECT_TRUE(sun::driver::isVoid(executeStringWithStdlib(R"(
    /** Completes without a scalar result. */
    function main() std.Result<void> { return std.Result.Ok; }
  )")));
}

/** String success uses the JIT's existing pointer-and-length return convention. */
TEST(EndToEnd_ResultMain, preserves_static_string) {
  const auto value = executeString(R"(
    /** Carries literal text or an error code. */
    enum Outcome { Ready(static_ptr<u8>), Failed(i32) }
    /** Returns literal text. */
    function main() Outcome { return Outcome.Ready("hello"); }
  )");
  EXPECT_EQ(std::get<std::string>(value), "hello");
}

/** The adapter forwards the original command-line arguments unchanged. */
TEST(EndToEnd_ResultMain, forwards_arguments) {
  char name[] = "test";
  char* argv[] = {name, nullptr};
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns the argument count through a result enum. */
    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) std.Result<i32> {
      return std.Result.Ok(argc);
    }
  )", 1, argv), 1);
}

/** An error without a reporting interface still produces a diagnostic and fails. */
TEST(EndToEnd_ResultMain, unreportable_error_exits_one) {
  EXPECT_EXIT(executeString(R"(
    /** Carries a scalar or an error code. */
    enum Outcome { Ready(i32), Failed(i32) }
    /** Reports failure at the process boundary. */
    function main() Outcome { return Outcome.Failed(7); }
  )"), testing::ExitedWithCode(1), "Unhandled main error: Failed");
}

/** Error reporting finishes and drops the error before terminating the process. */
TEST(EndToEnd_ResultMain, reports_and_drops_error) {
  EXPECT_EXIT(executeStringWithStdlib(R"(
    using std;
    /** Makes reporting and destruction observable. */
    class Problem implements IError {
      /** Identifies this error. */
      public const method code() i32 { return 7; }
      /** Supplies a readable description. */
      public const method message() AllocationResult<String> { return String("broken"); }
      /** Confirms that error ownership is released before exit. */
      deinit() { std.io.eprintln("dropped"); }
    }
    /** Carries a successful status or the owned error. */
    enum Outcome { Ready(i32), Failed(Problem) }
    /** Returns an owned failure. */
    function main() Outcome { return Outcome.Failed(Problem()); }
  )"), testing::ExitedWithCode(1), "broken\ndropped");
}

/** Diagnostic allocation failure uses fixed text without retrying allocation. */
TEST(EndToEnd_ResultMain, diagnostic_failure_has_fallback) {
  EXPECT_EXIT(executeStringWithStdlib(R"(
    using std;
    /** Always fails to allocate its diagnostic. */
    class Problem implements IError {
      /** Identifies this error. */
      public const method code() i32 { return 7; }
      /** Simulates exhausted diagnostic storage. */
      public const method message() AllocationResult<String> {
        return AllocationResult.Allocation(AllocationError(64));
      }
    }
    /** Carries successful completion or the error. */
    enum Outcome { Ready, Failed(Problem) }
    /** Returns the unformattable failure. */
    function main() Outcome { return Outcome.Failed(Problem()); }
  )"), testing::ExitedWithCode(1), "unable to allocate error message");
}

/** Owned class payloads cannot cross the native JIT entrypoint ABI. */
TEST(EndToEnd_ResultMain, rejects_unsupported_success) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** An owned value with no host return representation. */
    class Value { var number: i32 = 42; }
    /** Carries the unsupported success payload. */
    enum Outcome { Ready(Value), Failed(i32) }
    /** Returns an owned class. */
    function main() Outcome { return Outcome.Ready(Value()); }
  )"), "Unsupported success payload for 'main': 'Value'");
}

/** Ordinary entrypoints also reject values the host cannot safely receive. */
TEST(EndToEnd_ResultMain, rejects_unsupported_plain_return) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Owns a value with no host return representation. */
    class Value { var number: i32 = 42; }
    /** Returns an unsupported owned value directly. */
    function main() Value { return Value(); }
  )"), "Unsupported return type for 'main': 'Value'");
}

/** Native executable entrypoints cannot return a wide success payload. */
TEST(EndToEnd_ResultMain, aot_rejects_i64_success) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForAOT("result_main");
  driver->setExecutable(true);
  EXPECT_SUN_ERROR_WITH_MESSAGE(driver->compileString(R"(
    /** Carries a wide success value or failure code. */
    enum Outcome { Ready(i64), Failed(i32) }
    /** Returns a value unsuitable for a native exit code. */
    function main() Outcome { return Outcome.Ready(42i64); }
  )"), "compiled programs require i32 or void");
}

/** Boolean payloads retain their native JIT representation. */
TEST(EndToEnd_ResultMain, preserves_bool) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns a boolean success. */
    function main() std.Result<bool> { return std.Result.Ok(true); }
  )"), true);
}

/** Character payloads do not require an implicit integer conversion. */
TEST(EndToEnd_ResultMain, preserves_char) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    /** Returns a character success. */
    function main() std.Result<char> { return std.Result.Ok('A'); }
  )"), 65);
}

/** Calls to source main continue to return its enum rather than the adapter value. */
TEST(EndToEnd_ResultMain, recursive_calls_keep_result_type) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    var calls: i32 = 0;
    /** Calls the original function once, then unwraps its recursive result. */
    function main() std.Result<i32> {
      calls += 1;
      if (calls == 1) { return std.Result.Ok(try main()); }
      return std.Result.Ok(42);
    }
  )"), 42);
}

/** An entrypoint cannot expose multiple success values through one return slot. */
TEST(EndToEnd_ResultMain, rejects_multiple_success_payloads) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Carries an unsupported pair on success. */
    enum Outcome { Ready(i32, i32), Failed(i32) }
    /** Returns both values. */
    function main() Outcome { return Outcome.Ready(1, 2); }
  )"), "zero or one success payload");
}

/** Failure variants follow the same single-payload convention as try. */
TEST(EndToEnd_ResultMain, rejects_empty_failure_variant) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Has no error payload on failure. */
    enum Outcome { Ready(i32), Failed }
    /** Returns a supported scalar in an invalid result shape. */
    function main() Outcome { return Outcome.Ready(42); }
  )"), "each failure variant to have one payload");
}

/** Empty result success becomes the native executable's i32 return value. */
TEST(EndToEnd_ResultMain, aot_void_success_has_native_status) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForAOT("result_main_void");
  driver->setExecutable(true);
  driver->compileString(R"(
    /** Uses an empty first variant for successful completion. */
    enum Outcome { Ready, Failed(i32) }
    /** Completes successfully. */
    function main() Outcome { return Outcome.Ready; }
  )");
  auto* main = driver->getModule().getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(main->getReturnType()->isIntegerTy(32));
}
