// tests/end_to_end/test_testing.cpp - Running test functions end to end
//
// Uses executeTestsWithStdlib, which compiles like the test binary does:
// tests kept and made public, the runner main synthesized, stdlib linked.
// The runner's i32 result is the exit code: 0 when every test passed.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

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
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argvSeq)),
            0);
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
  EXPECT_EQ(executeTestsWithStdlib(program, 2, const_cast<char**>(argvSeq)),
            1);
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
TEST(EndToEnd_Testing, a_thrown_error_fails_the_test) {
  EXPECT_EQ(executeTestsWithStdlib(R"(
    using std;

    test_function throwsAnError() {
        throw Error(9, "kaboom");
    }

    test_function stillRuns() {
        std.test.assert(true);
    }
  )"),
            1);
}

// Test mode without any test functions is an error, not an empty pass.
TEST(EndToEnd_Testing, no_tests_found_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeTestsWithStdlib(R"(
    function main() i32 { return 0; }
  )"),
                                "no test functions found");
}

// Tests need std.test, so a test build without the standard library is
// refused with a pointer at stdlib.moon (same rule as interpolation).
TEST(EndToEnd_Testing, tests_without_stdlib_are_an_error) {
  initTestEnvironment();
  auto driver = Driver::createForJIT();
  driver->setTestHandling(Driver::TestHandling::Compile);
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
  EXPECT_NO_THROW(compileStringWithStdlib(R"(
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
  const char* argvUnion[] = {"program",       "--test-filter",
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
  initTestEnvironment();
  const char* root = std::getenv("SUN_PATH");
  ASSERT_NE(root, nullptr) << "SUN_PATH must point at the workspace root";
  EXPECT_NO_THROW(compileFileWithStdlib(
      (std::filesystem::path(root) / "tests/programs/testing_example.sun")
          .string()));
}
