// tests/stdlib/text/test_interpolation.cpp - Compile-environment tests for
// string interpolation. The runtime-behavior tests moved to Sun tests in
// stdlib/interpolation_tests.sun; what stays here are the cases a Sun test
// file cannot express: compiling without the stdlib at all, a compilation
// that declares std.String itself, and a program that never says `using std;`.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "driver/execution_utils.h"

TEST(Stdlib_Text_Interpolation, fails_without_stdlib) {
  // String interpolation requires stdlib for sun::String
  EXPECT_THROW(executeString(R"(
    function main() i64 {
        var x = 1;
        var s = `Value: ${x}`;
        return s.length();
    }
  )"),
               SunError);
}

TEST(Stdlib_Text_Interpolation, allowed_when_sources_declare_sun_string) {
  // No stdlib.moon: interpolation needs std.String and std.HeapAllocator to
  // exist, and this compilation declares them itself — the situation the
  // standard library's own sources are in.
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

TEST(Stdlib_Text_Interpolation, works_without_using_sun) {
  // String interpolation should work with just stdlib, no 'using std;' needed
  auto value = executeStringWithStdlib(R"(
    function main() i64 {
        var s = `Hello`;
        return s.length();
    }
  )");
  EXPECT_EQ(value, 5);
}
