// tests/test_pointers.cpp

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Pointer Type in Function Parameters
// ============================================================================

TEST(PointerTest, pass_static_ptr_to_function) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function foo(p: static_ptr<u8>) {
        return println(p);
    };

    function main() i32 {
        var x: static_ptr<u8> = "test";
        foo(x);
        return 0;
    };
  )");
  EXPECT_EQ(value, 0);
}

TEST(PointerTest, main_with_argc_argv) {
  const char* args[] = {"test_prog", "arg1", "arg2", "arg3", nullptr};
  auto value = executeString(
      R"(
    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) i32 {
        return argc;
    };
  )",
      4, const_cast<char**>(args));
  EXPECT_EQ(value, 4);
}
