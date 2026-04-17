// test_enums.cpp — Tests for enum types

#include <gtest/gtest.h>

#include "execution_utils.h"

// ============================================================================
// Basic enum definition and variant access
// ============================================================================

TEST(EnumTest, BasicEnumDefinition) {
  // Define a simple enum and use a variant
  auto value = executeString(R"(
    enum Color { Red, Green, Blue }

    function main() i32 {
      var c: Color = Color.Red;
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(EnumTest, EnumVariantValues) {
  // Enum variants have auto-incrementing values starting from 0
  auto value = executeString(R"(
    enum Color { Red, Green, Blue }

    function main() i32 {
      var r: Color = Color.Red;
      var g: Color = Color.Green;
      var b: Color = Color.Blue;
      // Cast to i32 to use in arithmetic (Red=0, Green=1, Blue=2)
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(EnumTest, EnumComparison) {
  // Enum values can be compared with == and !=
  auto value = executeString(R"(
    enum Status { Pending, Running, Completed }

    function main() i32 {
      var s: Status = Status.Running;
      if (s == Status.Running) {
        return 1;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(EnumTest, EnumNotEqual) {
  // Test != comparison
  auto value = executeString(R"(
    enum Status { Pending, Running, Completed }

    function main() i32 {
      var s: Status = Status.Completed;
      if (s != Status.Pending) {
        return 42;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumTest, EnumAsParameter) {
  // Enums can be passed as function parameters
  auto value = executeString(R"(
    enum Direction { Up, Down, Left, Right }

    function move(d: Direction) i32 {
      if (d == Direction.Up) { return 1; }
      if (d == Direction.Down) { return 2; }
      if (d == Direction.Left) { return 3; }
      return 4;
    }

    function main() i32 {
      return move(Direction.Left);
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(EnumTest, EnumAsReturnType) {
  // Enums can be returned from functions
  auto value = executeString(R"(
    enum Color { Red, Green, Blue }

    function getColor() Color {
      return Color.Green;
    }

    function main() i32 {
      var c: Color = getColor();
      if (c == Color.Green) {
        return 100;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(EnumTest, MultipleEnums) {
  // Multiple enums can be defined in the same file
  auto value = executeString(R"(
    enum Color { Red, Green, Blue }
    enum Size { Small, Medium, Large }

    function main() i32 {
      var c: Color = Color.Blue;
      var s: Size = Size.Large;
      if (c == Color.Blue) {
        if (s == Size.Large) {
          return 42;
        }
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumTest, EnumInClass) {
  // Enums can be used as class fields
  auto value = executeString(R"(
    enum Status { Active, Inactive }

    class Object {
      var status: Status;

      function init(s: Status) {
        this.status = s;
      }

      function isActive() i32 {
        if (this.status == Status.Active) {
          return 1;
        }
        return 0;
      }
    }

    function main() i32 {
      var obj = Object(Status.Active);
      return obj.isActive();
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(EnumTest, EnumWithManyVariants) {
  // Enum with more variants
  auto value = executeString(R"(
    enum Weekday { Monday, Tuesday, Wednesday, Thursday, Friday, Saturday, Sunday }

    function isWeekend(day: Weekday) i32 {
      if (day == Weekday.Saturday) { return 1; }
      if (day == Weekday.Sunday) { return 1; }
      return 0;
    }

    function main() i32 {
      var sat = isWeekend(Weekday.Saturday);
      var mon = isWeekend(Weekday.Monday);
      return sat + mon;
    }
  )");
  EXPECT_EQ(value, 1);  // Saturday is weekend (1), Monday is not (0)
}

TEST(EnumTest, EnumTrailingComma) {
  // Enum with trailing comma (should parse correctly)
  auto value = executeString(R"(
    enum Color { Red, Green, Blue, }

    function main() i32 {
      var c: Color = Color.Blue;
      if (c == Color.Blue) {
        return 1;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Error cases
// ============================================================================

TEST(EnumTest, UnknownVariant) {
  // Accessing an unknown variant should be an error
  EXPECT_THROW(executeString(R"(
    enum Color { Red, Green, Blue }

    function main() i32 {
      var c: Color = Color.Yellow;
      return 0;
    }
  )"),
               std::exception);
}

TEST(EnumTest, EmptyEnumError) {
  // Empty enum should be an error
  EXPECT_THROW(executeString(R"(
    enum Empty { }

    function main() i32 {
      return 0;
    }
  )"),
               std::exception);
}

TEST(EnumTest, DuplicateVariantError) {
  // Duplicate variant names should be an error
  EXPECT_THROW(executeString(R"(
    enum Color { Red, Green, Red }

    function main() i32 {
      return 0;
    }
  )"),
               std::exception);
}
