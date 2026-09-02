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
