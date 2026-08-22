// tests/memory_safety/test_pointers.cpp

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

// ============================================================================
// Pointer Type in Function Parameters
// ============================================================================

TEST(MemorySafety_Pointers, pass_static_ptr_to_function) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    function foo(p: static_ptr<u8>) void {
        println(p);
    };

    function main() i32 {
        var x: static_ptr<u8> = "test";
        foo(x);
        return 0;
    };
  )");
  EXPECT_EQ(value, 0);
}

TEST(MemorySafety_Pointers, main_with_argc_argv) {
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

// ============================================================================
// raw_ptr has no members: it is read through _load<T> / _to_ref<T>
// ============================================================================

TEST(MemorySafety_Pointers, raw_ptr_has_no_members) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var x: i32 = 7;
      var p: raw_ptr<i32> = _address_of<i32>(x);
      return p.get;
    }
  )"),
                                "raw_ptr has no member 'get'");
}
