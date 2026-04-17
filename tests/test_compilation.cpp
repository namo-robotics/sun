// tests/test_compilation.cpp
// Tests for AOT compilation features

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "driver.h"

// Helper function to test compilation (without JIT)
void compileString(const std::string& source) {
  Driver::createForAOT()->compileString(source);
}

// === Valid compilation tests ===

TEST(CompilationTest, main_returns_i32) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
        0;
    };
  )"));
}

TEST(CompilationTest, main_returns_i32_with_computation) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
        var x: i32 = 10;
        var y: i32 = 20;
        x + y;
    };
  )"));
}

TEST(CompilationTest, main_returns_i32_with_functions) {
  EXPECT_NO_THROW(compileString(R"(
function add(a: i32, b: i32) i32 {
        a + b;
    };

    function main() i32 {
        add(1, 2);
    };
  )"));
}

// === Invalid compilation tests (main must return i32) ===

TEST(CompilationTest, main_returns_f64_fails) {
  EXPECT_THROW(compileString(R"(
        function main() f64 {
            3.14;
        };
      )"),
               SunError);
}

TEST(CompilationTest, main_returns_f32_fails) {
  EXPECT_THROW(compileString(R"(
        function main() f32 {
            3.14;
        };
      )"),
               SunError);
}

TEST(CompilationTest, main_returns_i64_fails) {
  EXPECT_THROW(compileString(R"(
        function main() i64 {
            42;
        };
      )"),
               SunError);
}

TEST(CompilationTest, main_returns_bool_fails) {
  EXPECT_THROW(compileString(R"(
        function main() bool {
            1 < 2;
        };
      )"),
               SunError);
}

TEST(CompilationTest, main_returns_string_fails) {
  EXPECT_THROW(compileString(R"(
        function main() static_ptr<u8> {
            "hello";
        };
      )"),
               SunError);
}

// Test that error message is informative
TEST(CompilationTest, error_message_contains_type_info) {
  try {
    compileString(R"(
      function main() f64 {
          3.14;
      };
    )");
    FAIL() << "Expected SunError";
  } catch (const SunError& e) {
    std::string msg = e.what();
    EXPECT_TRUE(msg.find("i32") != std::string::npos)
        << "Error should mention i32";
    EXPECT_TRUE(msg.find("f64") != std::string::npos)
        << "Error should mention f64";
  }
}
