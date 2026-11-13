// tests/end_to_end/test_testing.cpp - Running test functions end to end
//
// Uses executeTestsWithStdlib, which compiles like the test binary does:
// tests kept and made public, the runner main synthesized, stdlib linked.
// The runner's i32 result is the exit code: 0 when every test passed.

#include <gtest/gtest.h>

#include <filesystem>

#include "driver/driver.h"
#include "driver/execution_utils.h"

using sun::driver::executeTestsWithStdlib;

// Every test passes: exit code 0, in both parallel and sequential modes.
TEST(EndToEnd_Testing, all_passing_tests_exit_zero) {
  const std::string program = R"(
    using std;

    module geometry {
        function addOne(n: i32) i32 {
            return n + 1;
        }

        test_function addsOne() {
             std.test.assert_eq(geometry.addOne(1), 2);
        }
    }

    test_function rootAssertHolds() {
         std.test.assert(true);
    }
  )";
  EXPECT_EQ(executeTestsWithStdlib(program), 0);

  const char* argvSeq[] = {"program", "--test-sequential", nullptr};
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argvSeq)), 0);
}

// One failing assertion turns the exit code to 1 without stopping the
// other tests, in both modes.
TEST(EndToEnd_Testing, a_failing_test_exits_one) {
  const std::string program = R"(
    using std;

    test_function passes() {
         std.test.assert(true);
    }

    test_function fails() {
         std.test.assert_eq(1, 2);
    }
  )";
  EXPECT_EQ(executeTestsWithStdlib(program), 1);

  const char* argvSeq[] = {"program", "--test-sequential", nullptr};
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argvSeq)), 1);
}

// A test sits inside the module it exercises, so module-scoped privacy lets
// it call the module's private helpers.
TEST(EndToEnd_Testing, tests_reach_module_private_helpers) {
  EXPECT_EQ(executeTestsWithStdlib(R"(
    using std;

    module counter {
        var count: i64 = 0;

        function bump() i64 {
            counter.count = counter.count + 1;
            return counter.count;
        }

        test_function bumps() {
             std.test.assert_eq(counter.bump(), counter.count);
        }
    }
  )"),
            0);
}

// The fixture idiom: a failed assertion throws, unwinding runs the same
// drop glue as scope exit, so the fixture's deinit (the teardown) still
// runs — observable through a module global the summary test reads.
TEST(EndToEnd_Testing, fixture_teardown_runs_on_failure) {
  // Sequential mode guarantees the first test's teardown happened before
  // the second test reads the counter.
  static const char* argvSeq[] = {"program", "--test-sequential", nullptr};
  EXPECT_EQ(executeTestsWithStdlib(R"(
    using std;

    module fx {
        var teardowns: i64 = 0;

        class Fixture {
            var dummy: i64;
            init() { this.dummy = 1; }
            deinit() { fx.teardowns = fx.teardowns + 1; }
        }

        // Tests live in the module so they reach its private Fixture.
        test_function failsButCleansUp() {
            var f = fx.Fixture();
             std.test.fail("intentional");
        }

        test_function teardownRan() {
             std.test.assert_eq(fx.teardowns, 1);
        }
    }
  )",
                                   2, const_cast<char**>(argvSeq)),
            1);  // failsButCleansUp fails on purpose, teardownRan passes
}

// An uncaught error from a test is a failure like any other, and a thrown
// error's message reaches the report path without crashing the runner.
TEST(EndToEnd_Testing, an_uncaught_exception_fails_the_test) {
  EXPECT_EQ(executeTestsWithStdlib(R"(
    using std;

    test_function returnsAnError() void throws {
        throw Error(9, "kaboom");
    }

    test_function stillRuns() {
         std.test.assert(true);
    }
  )"),
            1);
}

/** An empty runner succeeds rather than treating absence as a compiler error. */
TEST(EndToEnd_Testing, empty_test_suite_succeeds) {
  EXPECT_EQ(executeTestsWithStdlib("function main() i32 { return 0; }"), 0);
}

// Tests need std.test, so a test build without the standard library is
// refused with a pointer at stdlib.moon (same rule as interpolation).
TEST(EndToEnd_Testing, tests_without_stdlib_are_an_error) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForJIT();
  driver->setTestHandling(sun::driver::Driver::TestHandling::Compile);
  EXPECT_SUN_ERROR_WITH_MESSAGE(driver->executeString(R"(
    test_function lonely() { return; }
  )"),
                                "require the standard library");
}

// The user's main is replaced by the runner in a test build, and a program
// of only tests (no main at all) runs fine.
TEST(EndToEnd_Testing, user_main_is_replaced_and_optional) {
  EXPECT_EQ(executeTestsWithStdlib(R"(
    using std;

    function main() i32 {
        return 55;  // ignored: the runner takes main's place
    }

    test_function runs() {
         std.test.assert(true);
    }
  )"),
            0);

  EXPECT_EQ(executeTestsWithStdlib(R"(
    using std;

    test_function noMainNeeded() {
         std.test.assert(true);
    }
  )"),
            0);
}

// The production compile of the same source ignores tests entirely.
TEST(EndToEnd_Testing, production_compile_ignores_tests) {
  EXPECT_NO_THROW(sun::driver::compileStringWithStdlib(R"(
    using std;

    test_function ignored() {
         std.test.assert(true);
    }

    function main() i32 { return 0; }
  )"));
}

// One passing and one failing test make filter selection observable through
// the exit code: a run that selects only the passing test exits 0, one that
// reaches the failing test exits 1.
static const char* kFilterProgram = R"(
    using std;

    module geometry {
        test_function passes() {
             std.test.assert(true);
        }

        test_function alsoPasses() {
             std.test.assert(true);
        }
    }

    module physics {
        test_function fails() {
             std.test.assert(false);
        }
    }
)";

// --test-filter with the exact dotted name runs only that test.
TEST(EndToEnd_Testing, filter_exact_name_selects_one_test) {
  const char* argvPass[] = {"program", "--test-filter", "geometry.passes",
                            nullptr};
  EXPECT_EQ(
      executeTestsWithStdlib(kFilterProgram, 3, const_cast<char**>(argvPass)),
      0);

  const char* argvFail[] = {"program", "--test-filter", "physics.fails",
                            nullptr};
  EXPECT_EQ(
      executeTestsWithStdlib(kFilterProgram, 3, const_cast<char**>(argvFail)),
      1);
}

// A trailing star selects every test whose name starts with the prefix.
TEST(EndToEnd_Testing, filter_trailing_star_selects_a_module) {
  const char* argvGlob[] = {"program", "--test-filter", "geometry.*", nullptr};
  EXPECT_EQ(
      executeTestsWithStdlib(kFilterProgram, 3, const_cast<char**>(argvGlob)),
      0);
}

// A bare module name selects everything under it, no star needed.
TEST(EndToEnd_Testing, filter_module_prefix_selects_a_module) {
  const char* argvPrefix[] = {"program", "--test-filter", "geometry", nullptr};
  EXPECT_EQ(
      executeTestsWithStdlib(kFilterProgram, 3, const_cast<char**>(argvPrefix)),
      0);
}

// Repeated --test-filter flags select the union of their matches.
TEST(EndToEnd_Testing, repeated_filters_select_the_union) {
  const char* argvUnion[] = {"program",         "--test-filter",
                             "geometry.passes", "--test-filter",
                             "physics.fails",   nullptr};
  EXPECT_EQ(
      executeTestsWithStdlib(kFilterProgram, 5, const_cast<char**>(argvUnion)),
      1);
}

// A filter that matches nothing runs zero tests and still exits 0.
TEST(EndToEnd_Testing, filter_with_no_match_exits_zero) {
  const char* argvNone[] = {"program", "--test-filter", "nothing.here",
                            nullptr};
  EXPECT_EQ(
      executeTestsWithStdlib(kFilterProgram, 3, const_cast<char**>(argvNone)),
      0);
}

// Filtering composes with sequential mode.
TEST(EndToEnd_Testing, filter_combines_with_sequential) {
  const char* argvSeq[] = {"program", "--test-sequential", "--test-filter",
                           "geometry.*", nullptr};
  EXPECT_EQ(
      executeTestsWithStdlib(kFilterProgram, 4, const_cast<char**>(argvSeq)),
      0);
}

// A whole program file with inline tests plus a test_files manifest entry:
// the production compile strips the inline tests and never loads the test
// file. (Requires SUN_PATH at the workspace root.)
TEST(EndToEnd_Testing, example_program_compiles_in_production) {
  sun::driver::initTestEnvironment();
  const char* root = std::getenv("SUN_PATH");
  ASSERT_NE(root, nullptr) << "SUN_PATH must point at the workspace root";
  EXPECT_NO_THROW(sun::driver::compileFileWithStdlib(
      (std::filesystem::path(root) / "tests/programs/testing_example.sun")
          .string()));
}

/** Uncaught concrete exceptions fail tests in both runner modes. */
TEST(EndToEnd_Testing, uncaught_exceptions_fail_both_runner_modes) {
  const std::string program = R"(
    using std;
    /** Provides a concrete alternate failure. */
    exception Failure implements IError {
      /** Constructs the marker. */
      init() {}
      /** Identifies the failure. */
      public const method code() i32 { return 42; }
      /** Returns an owned diagnostic message. */
      public const method message() String throws { return String("returned failure"); }
    }
    /** Succeeds through its implicit unit result. */
    test_function passes() void throws {}
    /** Succeeds through an early bare return. */
    test_function early() void throws { return; }
    /** Reports a concrete error through the runner's common interface. */
    test_function fails() void throws { throw Failure(); }
  )";
  EXPECT_EQ(executeTestsWithStdlib(program), 1);
  const char* argv[] = {"program", "--test-sequential", nullptr};
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argv)), 1);
}

/** Tests pass with implicit or explicit void returns in both runner modes. */
TEST(EndToEnd_Testing, void_tests_support_implicit_and_explicit_return) {
  const std::string program = R"(
    using std;
    /** Returns success implicitly. */
    test_function implicit_success() {
       std.test.assert_eq(2 + 2, 4);
    }
    /** Returns success through a bare return. */
    test_function early_success() { return; }
    /** Accepts a result enum supplied by the standard library. */
    test_function library_success() void throws { return; }
    /** Coexists with the default builtin result convention. */
    test_function legacy_success() {  std.test.assert(true); }
  )";
  EXPECT_EQ(executeTestsWithStdlib(program), 0);
  const char* argv[] = {"program", "--test-sequential", nullptr};
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argv)), 0);
}

/** An assertion exception fails the test run. */
TEST(EndToEnd_Testing, assertion_exception_exits_one) {
  const std::string program = R"(
    using std;
    /** Propagates an assertion failure through a custom result. */
    test_function fails() {  std.test.assert(false, "custom failure"); }
  )";
  EXPECT_EQ(executeTestsWithStdlib(program), 1);
  const char* argv[] = {"program", "--test-sequential", nullptr};
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argv)), 1);
}

/** Checked arithmetic exceptions fail tests in both runner modes. */
TEST(EndToEnd_Testing, arithmetic_exception_exits_one) {
  const std::string program = R"(
    using std;
    /** Accepts the standard result's arithmetic variant on a successful test. */
    test_function succeeds() void throws { return; }
    /** Reports checked division failure through a custom result. */
    test_function fails() { var value =  std.checked_div(10, 0); }
  )";
  EXPECT_EQ(executeTestsWithStdlib(program), 1);
  const char* argv[] = {"program", "--test-sequential", nullptr};
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argv)), 1);
}

/** Failure payloads cannot silently evade the test runner's error matching. */
TEST(EndToEnd_Testing, rejects_non_void_test_return) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeTestsWithStdlib(R"(
    /** An integer is not a reportable test failure. */
    enum Outcome { Passed, Failure(i32) }
    /** Attempts to return an unsupported error payload. */
    test_function invalid() Outcome { return Outcome.Failure(1); }
  )"),
      "test functions return void");
}

/** Tests without annotations use void returns and allow exceptions. */
TEST(EndToEnd_Testing, default_void_test_contract) {
  EXPECT_EQ(executeTestsWithStdlib(R"(
    /** Returns the default library success explicitly. */
    test_function success() { return; }
  )"),
            0);
  EXPECT_EQ(executeTestsWithStdlib(R"(
    /** Returns the default library failure explicitly. */
    test_function failure() {
      throw std.test.AssertionError("explicit failure");
    }
  )"),
            1);
}

/** Check the public descriptor helpers from a module outside the standard
 * library. */
TEST(EndToEnd_Testing, public_errno_and_nonblocking_helpers) {
  EXPECT_EQ(executeTestsWithStdlib(R"(
    using std;
    using std.net;

    /** Exercise the descriptor API as an application importing stdlib would. */
    module descriptor_client {
        /** Read descriptor flags independently to check the public helper. */
        extern function fcntl(fd: i32, command: i32, ...) i32;

        /** Check flag changes, restoration, and error reporting across modules. */
        test_function descriptor_helpers() {
            var listener = TcpListener();
             listener.bind_loopback(0);
            var fd: i32 = listener.get_fd();
            var original: i32 = unsafe { fcntl(fd, 3); };
             std.test.assert(original >= 0);
             std.net.set_fd_nonblocking(fd, true);
            var enabled: i32 = unsafe { fcntl(fd, 3); };
             std.test.assert(enabled != original);
             std.net.set_fd_nonblocking(fd, false);
             std.test.assert_eq(unsafe { fcntl(fd, 3); }, original);

            var failed: i32 = unsafe { fcntl(-1, 3); };
            var code: i32 = std.errno();
             std.test.assert_eq(failed, -1);
             std.test.assert(code > 0);
             std.test.assert(not std.net.is_would_block(code));
            var reported: i32 = 0;
            try { std.net.set_fd_nonblocking(-1, true);
 } catch (e: Error) {  reported = e.code();  } catch (e: AllocationError) {  reported = e.code();  } catch (e: InvalidArgumentError) {  reported = e.code();  } catch (e: OverflowError) {  reported = e.code();  }
             std.test.assert_eq(reported, code);
        }
    }
  )"),
            0);
}

/** Arithmetic error results fail tests through the ordinary IError implementation. */
TEST(EndToEnd_Testing, concrete_arithmetic_errors_fail_tests) {
  EXPECT_EQ(executeTestsWithStdlib(R"(
    /** Propagates the standard-library failure from checked division. */
    test_function checked_failure() void throws {
      var value =  std.checked_div(10, 0);
      return;
    }
  )"), 1);
}
