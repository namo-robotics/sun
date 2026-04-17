// tests/test_generics.cpp - Tests for generic class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

TEST(Binops, int_float_mismatch) {
  EXPECT_THROW(executeString(R"(
      function main() f64 {
          var x: i32 = 0;
          var y: f64 = 1.0;
          return x + y;
      }
    )"),
               SunError);
}