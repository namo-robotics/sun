/** Checks compiler test helpers and per-instance native JIT bindings. */
#include <gtest/gtest.h>

#include "driver/driver.h"
#include "driver/execution_utils.h"

/** A declaration can be analyzed without supplying or executing main. */
TEST(Support_Analysis, accepts_declarations_without_main) {
  EXPECT_NO_THROW(sun::driver::analyzeString(R"(
    /** Returns a value if called by a separate executable. */
    function answer() i32 { return 42; }
  )"));
}

/** Parser failures returned by the driver must fail the calling test. */
TEST(Support_Analysis, throws_syntax_errors) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::analyzeString("function () i32 {}"), "function name");
}

/** An unresolved name must not be mistaken for successful analysis. */
TEST(Support_Analysis, throws_semantic_errors) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::analyzeString(R"(
    /** Refers to a name that has not been declared. */
    function answer() i32 { return missing_value; }
  )"),
                                "missing_value");
}

/** Earlier declarations and failures cannot leak into subsequent calls. */
TEST(Support_Analysis, isolates_calls) {
  ASSERT_NO_THROW(sun::driver::analyzeString(R"(
    /** Exists only in this analysis session. */
    class LocalType { var value: i32; }
  )"));
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::analyzeString(R"(
    /** Cannot refer to a type from an earlier session. */
    function consume(value: LocalType) void {}
  )"),
                                "LocalType");
  EXPECT_NO_THROW(sun::driver::analyzeString(R"(
    /** Reuses the name with a different shape in a fresh session. */
    class LocalType { var value: bool; }
  )"));
}

/** Optional library preloading resolves standard-library declarations. */
TEST(Support_Analysis, preloads_stdlib_when_requested) {
  EXPECT_NO_THROW(sun::driver::analyzeString(R"(
    /** Borrows a library type without allocating or executing anything. */
    function consume(value: const ref std.String) void {}
  )",
                                             true));
}

/** Duplicate native bindings produce a diagnostic instead of aborting the host. */
TEST(Support_JIT, duplicate_native_symbol_is_diagnosed) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForJIT();
  int value = 42;
  driver->defineJITSymbol("test_native_value", &value);
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      driver->defineJITSymbol("test_native_value", &value),
      "Cannot bind native JIT symbol 'test_native_value'");
}
