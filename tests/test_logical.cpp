// tests/test_logical.cpp - Tests for logical operators (and, or)

#include <gtest/gtest.h>

#include "execution_utils.h"

TEST(Logical, or_true_true) {
  auto value = executeString(R"(
      function main() i32 {
          if (true or true) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, or_true_false) {
  auto value = executeString(R"(
      function main() i32 {
          if (true or false) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, or_false_true) {
  auto value = executeString(R"(
      function main() i32 {
          if (false or true) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, or_false_false) {
  auto value = executeString(R"(
      function main() i32 {
          if (false or false) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(Logical, and_true_true) {
  auto value = executeString(R"(
      function main() i32 {
          if (true and true) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, and_true_false) {
  auto value = executeString(R"(
      function main() i32 {
          if (true and false) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(Logical, and_false_true) {
  auto value = executeString(R"(
      function main() i32 {
          if (false and true) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(Logical, and_false_false) {
  auto value = executeString(R"(
      function main() i32 {
          if (false and false) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 0);
}

TEST(Logical, or_short_circuit) {
  // If or short-circuits, we shouldn't call the second expression
  // We can test this by using a function that modifies state
  auto value = executeString(R"(
      var called: i32 = 0;
      
      function setFlag() bool {
          called = 1;
          return true;
      }
      
      function main() i32 {
          // true or setFlag() should NOT call setFlag
          if (true or setFlag()) {
              return called;  // Should be 0 if short-circuited
          }
          return -1;
      }
    )");
  EXPECT_EQ(value, 0);  // setFlag should not have been called
}

TEST(Logical, and_short_circuit) {
  // If and short-circuits, we shouldn't call the second expression
  auto value = executeString(R"(
      var called: i32 = 0;
      
      function setFlag() bool {
          called = 1;
          return true;
      }
      
      function main() i32 {
          // false and setFlag() should NOT call setFlag
          if (false and setFlag()) {
              return -1;
          }
          return called;  // Should be 0 if short-circuited
      }
    )");
  EXPECT_EQ(value, 0);  // setFlag should not have been called
}

TEST(Logical, chained_or) {
  auto value = executeString(R"(
      function main() i32 {
          if (false or false or true) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, chained_and) {
  auto value = executeString(R"(
      function main() i32 {
          if (true and true and true) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, mixed_and_or) {
  // or has lower precedence than and
  // so "true or true and false" is "true or (true and false)" = "true or false"
  // = true
  auto value = executeString(R"(
      function main() i32 {
          if (true or true and false) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, with_comparisons) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 5;
          var y: i32 = 10;
          if (x < 3 or y > 5) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}

TEST(Logical, complex_condition) {
  auto value = executeString(R"(
      function main() i32 {
          var x: i32 = 5;
          var y: i32 = 10;
          // (5 >= 5) and ((10 < 20) or (5 == 0))
          // true and (true or false)
          // true and true = true
          if (x >= 5 and (y < 20 or x == 0)) { return 1; }
          return 0;
      }
    )");
  EXPECT_EQ(value, 1);
}
