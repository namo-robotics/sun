// tests/enums/test_enums.cpp - Tests for enum types

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// ============================================================================
// Basic enum definition and variant access
// ============================================================================

TEST(Enums, BasicEnumDefinition) {
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

TEST(Enums, EnumVariantValues) {
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

TEST(Enums, EnumComparison) {
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

TEST(Enums, EnumNotEqual) {
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

TEST(Enums, EnumAsParameter) {
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

TEST(Enums, EnumAsReturnType) {
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

TEST(Enums, MultipleEnums) {
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

TEST(Enums, EnumInClass) {
  // Enums can be used as class fields
  auto value = executeString(R"(
    enum Status { Active, Inactive }

    class Object {
      var status: Status;

      init(s: Status) {
        this.status = s;
      }

      method isActive() i32 {
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

TEST(Enums, EnumWithManyVariants) {
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

TEST(Enums, EnumTrailingComma) {
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

TEST(Enums, UnknownVariant) {
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

TEST(Enums, EmptyEnumError) {
  // Empty enum should be an error
  EXPECT_THROW(executeString(R"(
    enum Empty { }

    function main() i32 {
      return 0;
    }
  )"),
               std::exception);
}

TEST(Enums, DuplicateVariantError) {
  // Duplicate variant names should be an error
  EXPECT_THROW(executeString(R"(
    enum Color { Red, Green, Red }

    function main() i32 {
      return 0;
    }
  )"),
               std::exception);
}

// ============================================================================
// Variants named through a module path
// ============================================================================

TEST(Enums, QualifiedUnitVariantThroughModulePath) {
  // A unit variant reached through its module path works everywhere the bare
  // form does: initializer, comparison, argument, annotation, return value
  auto value = executeString(R"(
    public module a.b {
      public enum E { X, Y }
      public function is_y(e: E) i32 { if (e == E.Y) { return 1; } return 0; }
    }

    function flip(e: a.b.E) a.b.E {
      if (e == a.b.E.X) { return a.b.E.Y; }
      return a.b.E.X;
    }

    function main() i32 {
      var e = a.b.E.Y;
      var total: i32 = 0;
      if (e == a.b.E.Y) { total += 1; }
      total += a.b.is_y(a.b.E.Y);
      var x: a.b.E = a.b.E.X;
      if (x != a.b.E.Y) { total += 1; }
      if (flip(x) == a.b.E.Y) { total += 1; }
      return total;
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(Enums, QualifiedUnitVariantMatchPatterns) {
  // Match arms may spell the variant through the module path too
  auto value = executeString(R"(
    public module a.b { public enum E { X, Y, Z } }

    function main() i32 {
      var e = a.b.E.Y;
      return match e {
        a.b.E.X => 1,
        a.b.E.Y => 2,
        a.b.E.Z => 3
      };
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Enums, QualifiedUnitVariantFromLibraryModulePath) {
  // std.io.FileMode.Write names the stdlib enum's variant with no
  // `using std.io;` import, the same way std.io.File names the class
  auto value = executeStringWithStdlib(R"(
    using std;

    function pick(m: std.io.FileMode) i32 {
      if (m == std.io.FileMode.Write) { return 1; }
      return 0;
    }

    function main() i32 {
      var m = std.io.FileMode.Write;
      return pick(m) + pick(std.io.FileMode.Write) + pick(std.io.FileMode.Read);
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Enums, UnknownLibraryModuleMemberNamesSourceModule) {
  // The diagnostic spells the module as the source does, without the
  // bundle's hash scope
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;

    function main() i32 {
      var x = std.io.Nope;
      return 0;
    }
  )"),
                                "Unknown member 'Nope' in module 'std.io'");
}

TEST(Enums, QualifiedPrivatePayloadVariantDenied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m { enum Secret { Item(i32) } }
    function main() i32 { var s = m.Secret.Item(1); return 0; }
  )"),
                                "enum 'Secret' is private to module 'm'");
}

TEST(Enums, QualifiedVariantThroughPrivateModuleDenied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m {
      module hidden { public enum E { Item(i32) } }
    }
    function main() i32 { var s = m.hidden.E.Item(1); return 0; }
  )"),
                                "module 'hidden' is private to module 'm'");
}

TEST(Enums, QualifiedPrivateEnumPatternDenied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m {
      enum Secret { Item(i32) }
      public function make() Secret { return Secret.Item(1); }
    }
    function main() i32 {
      var s = m.make();
      return match s { m.Secret.Item(v) => v };
    }
  )"),
                                "enum 'Secret' is private to module 'm'");
}
