// tests/builtins/test_static_ptr.cpp - static_ptr<T> accessors (issue #95)
//
// A static_ptr<T> is a { ptr, i64 } fat pointer. Its supported accessors are
// the methods length() and raw(); they replaced the _static_ptr_len<T> and
// _static_ptr_data<T> intrinsics.

#include <gtest/gtest.h>

#include "execution_utils.h"

static std::string capturePrintedOutput(const std::string& source) {
  testing::internal::CaptureStdout();
  executeString(source);
  return testing::internal::GetCapturedStdout();
}

// ============================================================================
// length()
// ============================================================================

TEST(Builtins_StaticPtr, length_of_a_literal) {
  auto value = executeString(R"(
    function main() i32 {
      return _convert<i32>("hello".length());
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Builtins_StaticPtr, length_of_a_local) {
  auto value = executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      return _convert<i32>(s.length());
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Builtins_StaticPtr, length_of_a_by_value_parameter) {
  auto value = executeString(R"(
    function count(s: static_ptr<u8>) i64 { return s.length(); }
    function main() i32 {
      return _convert<i32>(count("four"));
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(Builtins_StaticPtr, length_through_a_ref_parameter) {
  auto value = executeString(R"(
    function count(s: ref static_ptr<u8>) i64 { return s.length(); }
    function main() i32 {
      var s: static_ptr<u8> = "twelve chars";
      return _convert<i32>(count(s));
    }
  )");
  EXPECT_EQ(value, 12);
}

TEST(Builtins_StaticPtr, length_of_a_class_field) {
  auto value = executeString(R"(
    class Named {
      var name: static_ptr<u8>;
      function init() { this.name = "wxyz"; }
      function name_length() i64 { return this.name.length(); }
    }
    function main() i32 {
      var n: Named = Named();
      return _convert<i32>(n.name_length());
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(Builtins_StaticPtr, length_of_a_method_result) {
  auto value = executeString(R"(
    class Boom implements IError {
      function init() {}
      function code() i32 { return 9; }
      function message() static_ptr<u8> { return "boom"; }
    }
    function main() i32 {
      var b: Boom = Boom();
      return b.code() + _convert<i32>(b.message().length());
    }
  )");
  EXPECT_EQ(value, 13);
}

TEST(Builtins_StaticPtr, empty_literal_has_length_zero) {
  auto value = executeString(R"(
    function main() i32 {
      return _convert<i32>("".length());
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// raw()
// ============================================================================

TEST(Builtins_StaticPtr, raw_and_length_feed_print_bytes) {
  auto out = capturePrintedOutput(R"(
    function main() i32 {
      var s: static_ptr<u8> = "hi!";
      _print_bytes(s.raw(), s.length());
      return 0;
    }
  )");
  EXPECT_EQ(out, "hi!");
}

TEST(Builtins_StaticPtr, raw_is_a_raw_ptr_to_the_bytes) {
  auto value = executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "A";
      var p: raw_ptr<u8> = s.raw();
      return _convert<i32>(_load<u8>(p, 0));
    }
  )");
  EXPECT_EQ(value, 65);
}

TEST(Builtins_StaticPtr, raw_passes_a_literal_to_c) {
  auto value = executeString(R"(
    extern function strlen(s: raw_ptr<u8>) i64;
    function main() i64 {
      var s: static_ptr<u8> = "hello";
      unsafe { return strlen(s.raw()); };
    }
  )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// Only the method form exists
// ============================================================================

TEST(Builtins_StaticPtr, property_form_of_length_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      var n: i64 = s.length;
      return 0;
    }
  )"),
                                "call 'length()'");
}

TEST(Builtins_StaticPtr, property_form_of_raw_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      var p: raw_ptr<u8> = s.raw;
      return 0;
    }
  )"),
                                "call 'raw()'");
}

TEST(Builtins_StaticPtr, accessors_take_no_arguments) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      return _convert<i32>(s.length(1));
    }
  )"),
                                "static_ptr.length() takes no arguments");
}

TEST(Builtins_StaticPtr, unknown_method_lists_the_accessors) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      return _convert<i32>(s.size());
    }
  )"),
                                "available: length(), raw()");
}

TEST(Builtins_StaticPtr, old_data_name_is_not_a_member) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      var p: raw_ptr<u8> = s.data;
      return 0;
    }
  )"),
                                "static_ptr has no member 'data'");
}

TEST(Builtins_StaticPtr, removed_intrinsics_are_unknown) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      return _convert<i32>(_static_ptr_len<u8>(s));
    }
  )"),
                                "_static_ptr_len");
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
      var s: static_ptr<u8> = "abc";
      var p: raw_ptr<u8> = _static_ptr_data<u8>(s);
      return 0;
    }
  )"),
                                "_static_ptr_data");
}
