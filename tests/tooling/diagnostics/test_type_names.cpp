// tests/tooling/diagnostics/test_type_names.cpp — error messages name types
// the way the source spells them.
//
// A moon import prefixes every symbol with the bundle's content hash, so a
// stdlib class is called "$724e4cc5$_sun_Vec" internally. Diagnostics must
// print the display name ("sun.Vec<u8>") instead — see issue #135.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

namespace {

// Compile `source` expecting failure, and return the error message.
std::string errorFor(const std::string& source) {
  try {
    compileString(source, /*includeStdlib=*/true);
  } catch (const SunError& e) {
    return e.what();
  }
  ADD_FAILURE() << "expected the program to fail to compile";
  return "";
}

// Every mangled name carries the bundle hash between two '$'.
void expectNoMangledName(const std::string& message) {
  EXPECT_EQ(message.find('$'), std::string::npos)
      << "diagnostic leaks a mangled name: " << message;
}

void expectNames(const std::string& message, const std::string& expected) {
  EXPECT_NE(message.find(expected), std::string::npos)
      << "expected '" << expected << "' in: " << message;
  expectNoMangledName(message);
}

}  // namespace

TEST(Tooling_Diagnostics_TypeNames, lambda_capture_of_moon_class) {
  std::string message = errorFor(R"(
    using sun;
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<u8>(alloc, 16);
      var t = spawn(lambda() i64 { return v.length(); });
      return 0;
    }
  )");
  expectNames(message, "compound type 'sun.Vec<u8>'");
}

TEST(Tooling_Diagnostics_TypeNames, assignment_mismatch) {
  std::string message = errorFor(R"(
    using sun;
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<u8>(alloc, 16);
      var x: i32 = v;
      return 0;
    }
  )");
  expectNames(message, "value of type 'sun.Vec<u8>'");
}

TEST(Tooling_Diagnostics_TypeNames, unknown_member_on_moon_class) {
  std::string message = errorFor(R"(
    using sun;
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<u8>(alloc, 16);
      v.no_such_method();
      return 0;
    }
  )");
  expectNames(message, "on class 'sun.Vec<u8>'");
}

TEST(Tooling_Diagnostics_TypeNames, constructor_argument_list) {
  std::string message = errorFor(R"(
    using sun;
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<u8>(alloc, "not a size");
      return 0;
    }
  )");
  expectNames(message, "No matching constructor for 'sun.Vec<u8>'");
  // Pointer types are spelled the way the source spells them, too
  expectNames(message, "static_ptr<u8>");
}

TEST(Tooling_Diagnostics_TypeNames, catch_type_must_implement_ierror) {
  std::string message = errorFor(R"(
    using sun;
    function main() i32 {
      try { var z = 1; } catch (e: Vec<u8>) { return 1; }
      return 0;
    }
  )");
  expectNames(message, "got 'sun.Vec<u8>'");
}

TEST(Tooling_Diagnostics_TypeNames, ternary_branch_mismatch) {
  std::string message = errorFor(R"(
    using sun;
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<u8>(alloc, 16);
      var s = String("hi");
      var r = true ? v : s;
      return 0;
    }
  )");
  expectNames(message, "'sun.Vec<u8>' vs 'String'");
}

TEST(Tooling_Diagnostics_TypeNames, enum_payload_mismatch) {
  std::string message = errorFor(R"(
    using sun;
    enum E { A(Vec<u8>), B }
    function main() i32 {
      var e = E.A(1);
      return 0;
    }
  )");
  expectNames(message, "expected 'sun.Vec<u8>'");
}
