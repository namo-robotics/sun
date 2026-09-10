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

TEST(Enums, ExplicitValuesAndImplicitSuccessors) {
  EXPECT_EQ(executeString(R"(
    enum Kind { Zero, Data = 21, Next, Heartbeat = 7, Negative = -2, Last }
    function main() i32 {
      var kind = Kind.Next;
      var matched = match kind {
        Kind.Zero => 0, Kind.Data => 1, Kind.Next => 42,
        Kind.Heartbeat => 3, Kind.Negative => 4, Kind.Last => 5
      };
      if (_convert<i32>(Kind.Data) != 21) { return 1; }
      if (_convert<i64>(Kind.Negative) != -2) { return 2; }
      if (_convert<i64>(Kind.Last) != -1) { return 3; }
      return matched;
    }
  )"),
            42);
}

TEST(Enums, ExplicitIntegerBoundaries) {
  EXPECT_EQ(executeString(R"(
    enum Kind { Low = -2147483648, High = 2147483647, Hex = 0x15 }
    function main() i32 {
      if (_convert<i64>(Kind.Low) != -2147483648) { return 1; }
      if (_convert<i64>(Kind.High) != 2147483647) { return 2; }
      return _convert<i32>(Kind.Hex);
    }
  )"),
            21);
}

TEST(Enums, RejectInvalidExplicitValues) {
  for (const auto& declaration :
       {"enum E { A = 1, B = 1 }", "enum E { A, B = 0 }",
        "enum E { A = 2147483648 }", "enum E { A = -2147483649 }",
        "enum E { A = 2147483647, B }", "enum E { A = 18446744073709551615 }",
        "enum E { A = 1.5 }", "enum E { A = 1 + 2 }",
        "enum E { A = 1, B(i32) }", "enum E<T> { A = 1, B(T) }"}) {
    EXPECT_THROW(compileString(std::string(declaration) +
                               " function main() i32 { return 0; }"),
                 std::exception)
        << declaration;
  }
}

TEST(Enums, CheckedIntegerDecoding) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    enum Kind { Data = 21, Heartbeat = 7, Negative = -1 }
    function decode<T>(value: T) i32 {
      return match _enum_from_int<Kind>(value) {
        Option.Some(kind) => match kind {
          Kind.Data => 1, Kind.Heartbeat => 2, Kind.Negative => 3
        },
        Option.None => 10
      };
    }
    function main() i32 {
      var byte: u8 = 21;
      var wide: u64 = 4294967317;
      var maximum: u64 = 18446744073709551615;
      return decode(byte) + decode(7) + decode(-1) + decode(8)
        + decode(wide) + decode(maximum);
    }
  )"),
            36);
}

TEST(Enums, CheckedIntegerDecodingRejectsInvalidTypes) {
  for (const auto& expression :
       {"_enum_from_int<i32>(1)", "_enum_from_int<E>(1.0)",
        "_enum_from_int<E>(true)", "_enum_from_int<P>(1)",
        "_enum_from_int<E>()", "_enum_from_int<E>(1, 2)"}) {
    EXPECT_THROW(compileString(std::string("enum E { A = 1 } enum P { A(i32) } "
                                           "function main() i32 { var x = ") +
                               expression + "; return 0; }"),
                 std::exception)
        << expression;
  }
}

TEST(Enums, CheckedDecodingWithGenericTargetAndReference) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    enum Kind { Data = 21 }
    function decode<T>(value: ref i32) Option<T> {
      return _enum_from_int<T>(value);
    }
    function main() i32 {
      var value = 21;
      var decoded = decode<Kind>(value);
      return match decoded {
        Option.Some(kind) => _convert<i32>(kind), Option.None => 0
      };
    }
  )"),
            21);
}

TEST(Enums, CheckedDecodingRejectsInvalidGenericArgument) {
  EXPECT_THROW(compileStringWithStdlib(R"(
    using std;
    enum Kind { Data = 21 }
    function decode<T>(value: T) Option<Kind> {
      return _enum_from_int<Kind>(value);
    }
    function main() i32 { var decoded = decode(21.0); return 0; }
  )"),
               std::exception);
}

TEST(Enums, NegativeTagDuplicateArmDoesNotAffectResultType) {
  EXPECT_EQ(executeString(R"(
    enum Kind { Negative = -1, Positive = 1 }
    function pick(kind: Kind) i32 {
      return match kind {
        Kind.Negative => { return 42; },
        Kind.Negative => 1.0,
        Kind.Positive => 7
      };
    }
    function main() i32 { return pick(Kind.Negative); }
  )"),
            42);
}

TEST(Enums, UnderlyingIntegerWidths) {
  for (const auto& name :
       {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"}) {
    const std::string type(name);
    const int size = std::stoi(type.substr(1)) / 8;
    EXPECT_EQ(executeString("enum Color " + type +
                            R"( { Red = 1, Green = 2, Blue = 3 }
      function main() i32 { return _convert<i32>(_sizeof<Color>()); }
    )"),
              size)
        << type;
  }
}

TEST(Enums, ByteEnumStorageAndReferenceMatch) {
  EXPECT_EQ(executeString(R"(
    enum Color u8 { Red = 1, Green, Blue }
    class Palette {
      var colors: array<Color, 3>;
      init() { this.colors = [Color.Red, Color.Green, Color.Blue]; }
    }
    function identify(color: ref Color) i32 {
      return match color { Color.Red => 1, Color.Green => 2, Color.Blue => 3 };
    }
    function main() i32 {
      var colors = Palette();
      if (_sizeof<Palette>() != 3) { return 10; }
      return identify(colors.colors[1]);
    }
  )"),
            2);
}

TEST(Enums, UnderlyingSignednessAndWideTags) {
  EXPECT_EQ(executeString(R"(
    enum Small i8 { Negative = -128, Next }
    enum Byte u8 { High = 255 }
    enum Wide u64 { Low = 1, High = 4294967297, Max = 18446744073709551615 }
    enum Signed i64 { Min = -9223372036854775808, Max = 9223372036854775807 }
    function identify(value: ref Wide) i32 {
      return match value { Wide.Low => 1, Wide.High => 2, Wide.Max => 3 };
    }
    function main() i32 {
      if (_convert<i64>(Small.Negative) != -128) { return 10; }
      if (_convert<i64>(Small.Next) != -127) { return 11; }
      if (_convert<i64>(Byte.High) != 255) { return 12; }
      if (_convert<u64>(Wide.Max) != 18446744073709551615u64) { return 13; }
      if (_convert<i64>(Signed.Min) != -9223372036854775808) { return 14; }
      if (_convert<i64>(Signed.Max) != 9223372036854775807) { return 15; }
      var value = Wide.High;
      return identify(value);
    }
  )"),
            2);
}

TEST(Enums, UnderlyingValueRangeErrors) {
  for (const auto& declaration : {"enum E u8 { A = -1 }",
                                  "enum E u8 { A = 256 }",
                                  "enum E i8 { A = -129 }",
                                  "enum E i8 { A = 128 }",
                                  "enum E u16 { A = 65536 }",
                                  "enum E i16 { A = 32768 }",
                                  "enum E u32 { A = 4294967296 }",
                                  "enum E i32 { A = -2147483649 }",
                                  "enum E i64 { A = 9223372036854775808 }",
                                  "enum E i64 { A = -9223372036854775809 }",
                                  "enum E u8 { A = 255, B }",
                                  "enum E i8 { A = 127, B }",
                                  "enum E u64 { A = 18446744073709551615, B }",
                                  "enum E i64 { A = 9223372036854775807, B }",
                                  "enum E u64 { A = -1 }",
                                  "enum E f32 { A }",
                                  "enum E bool { A }",
                                  "enum E char { A }",
                                  "enum E ref i32 { A }",
                                  "enum E u8 { A(i32) }"}) {
    EXPECT_THROW(compileString(std::string(declaration) +
                               " function main() i32 { return 0; }"),
                 std::exception)
        << declaration;
  }
}

TEST(Enums, UnsignedMaximumMayBeFollowedByExplicitReset) {
  EXPECT_EQ(executeString(R"(
    enum E u64 { Before = 18446744073709551614, Max, Zero = 0, One }
    function main() i32 {
      if (_convert<u64>(E.Max) != 18446744073709551615u64) { return 10; }
      return _convert<i32>(E.One);
    }
  )"),
            1);
}

TEST(Enums, CheckedDecodingUsesUnderlyingRangeAndSignedness) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    enum Byte u8 { High = 255 }
    enum Signed i8 { Negative = -1 }
    enum Wide u64 { Max = 18446744073709551615 }
    function byte_value<T>(value: T) i32 {
      return match _enum_from_int<Byte>(value) {
        Option.Some(e) => _convert<i32>(e), Option.None => 0
      };
    }
    function wide_value<T>(value: T) i32 {
      return match _enum_from_int<Wide>(value) {
        Option.Some(e) => match e { Wide.Max => 1 }, Option.None => 0
      };
    }
    function main() i32 {
      if (byte_value(255) != 255) { return 10; }
      if (byte_value(-1) != 0) { return 11; }
      if (byte_value(511) != 0) { return 12; }
      if (wide_value(-1) != 0) { return 13; }
      if (wide_value(18446744073709551615u64) != 1) { return 14; }
      var unsigned_byte: u8 = 255;
      var rejected = match _enum_from_int<Signed>(unsigned_byte) {
        Option.Some(e) => 1, Option.None => 0
      };
      if (rejected != 0) { return 15; }
      return match _enum_from_int<Signed>(-1) {
        Option.Some(e) => _convert<i32>(e), Option.None => 16
      };
    }
  )"),
            -1);
}

TEST(Enums, GenericEnumKeepsUnderlyingType) {
  EXPECT_EQ(executeString(R"(
    enum State<T> u8 { Empty = 0, Full = 255 }
    function main() i32 {
      var state: State<i32> = State.Full;
      if (_sizeof<State<i32>>() != 1) { return 10; }
      return _convert<i32>(state);
    }
  )"),
            255);
}
