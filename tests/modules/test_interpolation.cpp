// tests/modules/test_interpolation.cpp - Where an interpolated string's
// names come from
//
// A template literal desugars to std.String and std.HeapAllocator calls
// (parsing/lowering_pass.cpp), and those names resolve like any other module
// name: from stdlib.moon without the program writing `using std;`, from
// sources that declare the classes themselves (the standard library's own
// situation), and not at all when neither is present (driver.cpp). The
// runtime behavior of interpolation is tested in
// stdlib/interpolation_tests.sun.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Modules_Interpolation, fails_without_stdlib) {
  EXPECT_THROW(executeString(R"(
    function main() i64 {
        var x = 1;
        var s = `Value: ${x}`;
        return s.length();
    }
  )"),
               SunError);
}

TEST(Modules_Interpolation, allowed_when_sources_declare_sun_string) {
  // No stdlib.moon: interpolation needs std.String and std.HeapAllocator to
  // exist, and this compilation declares them itself.
  auto value = executeString(R"(
    public module std {
      public class HeapAllocator {
        init() {}
      }
      public class String {
        var len: i64;
        init(alloc: const ref HeapAllocator, literal: static_ptr<u8>) {
          this.len = literal.length();
        }
        public method append_literal(literal: static_ptr<u8>) void {
          this.len = this.len + literal.length();
        }
        public method append(value: i64) void {
          this.len = this.len + 1;
        }
        public method length() i64 { return this.len; }
      }
    }

    function main() i64 {
        var x: i64 = 7;
        var s = `n=${x}`;
        return s.length() - 3;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_Interpolation, works_without_using_sun) {
  auto value = executeStringWithStdlib(R"(
    function main() i64 {
        var s = `Hello`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Modules_Interpolation, hex_escapes_preserve_bytes_around_expressions) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    function main() i64 throws IError {
      var protocol = "http/1.1";
      var s = `\x08${protocol}\x00\x7f\x80\xfF\x41F\\x08`;
      if (s.length() != 19 or s.at(0) != 8) { return 1; }
      if (s.at(1) != b'h' or s.at(8) != b'1' or
          s.at(9) != 0 or s.at(10) != 127 or s.at(11) != 128 or
          s.at(12) != 255 or s.at(13) != b'A' or s.at(14) != b'F' or
          s.at(15) != b'\\' or s.at(16) != b'x' or
          s.at(17) != b'0' or s.at(18) != b'8') { return 2; }
      return 0;
    }
  )"),
            0);
}

TEST(Modules_Interpolation, hex_escapes_require_two_digits) {
  for (const auto* escape :
       {R"(\x)", R"(\x0)", R"(\xGG)", R"(\x0G)", R"(\x0${1})"}) {
    SCOPED_TRACE(escape);
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        executeStringWithStdlib(std::string("function main() i64 { var s = `") +
                                escape + "`; return s.length(); }"),
        "\\x needs exactly two hex digits");
  }
}

TEST(Modules_Interpolation, unused_generic_body_accepts_unbound_value) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    function label<T>(value: T) std.String { return `v=${value}`; }
    function main() i32 { return 0; }
  )"),
            0);
}

TEST(Modules_Interpolation, generic_interpolation_rejects_unsupported_type) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeStringWithStdlib(R"(
    class Unprintable { init() {} }
    function label<T>(value: T) std.String { return `v=${value}`; }
    function main() i32 {
      var text = label(Unprintable());
      return 0;
    }
  )"),
      "Type mismatch in argument 1 of call to 'append'");
}
