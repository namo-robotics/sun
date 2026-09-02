// tests/functions/test_test_functions.cpp - The test_function declaration
//
// Covers the parser rules for `test_function` (no parameters, no return
// type, implicit throws, item level only, never public) and the default
// production behavior: tests are stripped, so a normal build neither runs
// nor even analyzes them. Running tests end to end lives in
// EndToEnd_Testing.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// In a production build the test is stripped and main runs untouched.
TEST(TestFunctions, stripped_from_production_builds) {
  auto value = executeString(R"(
    test_function neverRuns() {
        return;
    }

    function main() i32 {
        return 7;
    }
  )");
  EXPECT_EQ(value, 7);
}

// Stripping happens before analysis: a test may reference names that do not
// exist in the production build (its helpers may live in test_files).
TEST(TestFunctions, stripped_before_analysis) {
  auto value = executeString(R"(
    test_function usesMissingHelper() {
        helperFromTestFiles();
    }

    function main() i32 {
        return 3;
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(TestFunctions, rejects_parameters) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    test_function bad(x: i32) {
        return;
    }
    function main() i32 { return 0; }
  )"),
                                "a test function takes no parameters");
}

TEST(TestFunctions, rejects_return_type) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    test_function bad() i32 {
        return 1;
    }
    function main() i32 { return 0; }
  )"),
                                "does not declare a return type");
}

// Every test may throw; spelling it is redundant and rejected.
TEST(TestFunctions, rejects_explicit_throws) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    test_function bad() throws IError {
        return;
    }
    function main() i32 { return 0; }
  )"),
                                "'throws IError' is implicit");
}

TEST(TestFunctions, rejects_public) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    public test_function bad() {
        return;
    }
    function main() i32 { return 0; }
  )"),
                                "cannot be 'public'");
}

TEST(TestFunctions, rejects_type_parameters) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    test_function bad<T>() {
        return;
    }
    function main() i32 { return 0; }
  )"),
                                "cannot take type parameters");
}

TEST(TestFunctions, rejects_name_main) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    test_function main() {
        return;
    }
  )"),
                                "cannot be named 'main'");
}

TEST(TestFunctions, rejects_tests_inside_classes) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Point {
        var x: i32;
        init(x: i32) { this.x = x; }
        test_function bad() { return; }
    }
    function main() i32 { return 0; }
  )"),
                                "not allowed inside classes");
}

TEST(TestFunctions, rejects_tests_inside_interfaces) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IShape {
        test_function bad() { return; }
    }
    function main() i32 { return 0; }
  )"),
                                "not allowed inside interfaces");
}

TEST(TestFunctions, rejects_tests_inside_function_bodies) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
        test_function bad() { return; }
        return 0;
    }
  )"),
                                "only allowed at module scope");
}

// Two same-named tests in one module have identical no-parameter signatures
// and hit the ordinary duplicate-declaration error. The build here is a test
// build, since production builds strip both before analysis.
TEST(TestFunctions, duplicate_names_in_one_module_collide) {
  EXPECT_THROW(executeTestsWithStdlib(R"(
    using std;
    module geometry {
        test_function same() { return; }
        test_function same() { return; }
    }
  )"),
               SunError);
}

// "test_function" is one token; an identifier merely starting with it stays
// an identifier.
TEST(TestFunctions, longer_identifiers_are_not_the_keyword) {
  auto value = executeString(R"(
    function test_functions() i32 {
        return 11;
    }

    function main() i32 {
        return test_functions();
    }
  )");
  EXPECT_EQ(value, 11);
}
