// tests/test_enum_payloads.cpp - Payload-carrying enums, match destructuring,
// and exhaustiveness checking

#include <gtest/gtest.h>

#include <fstream>

#include "execution_utils.h"
#include "metadata_extractor.h"
#include "moon/moon.h"
#include "moon_import.h"

// ============================================================================
// Construction + destructuring
// ============================================================================

TEST(EnumPayloadTest, ConstructAndMatchSinglePayload) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }

    function main() i32 {
      var x = Opt.Some(42);
      return match x {
        Opt.Some(v) => v,
        Opt.None => -1
      };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumPayloadTest, UnitVariantOfPayloadEnum) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }

    function main() i32 {
      var x = Opt.None;
      return match x {
        Opt.Some(v) => v,
        Opt.None => 7
      };
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(EnumPayloadTest, MultiPayloadAndWildcardBinding) {
  auto value = executeString(R"(
    enum Shape { Circle(f64), Rect(f64, f64), Empty }

    function main() i32 {
      var r = Shape.Rect(3.0, 4.0);
      var area = match r {
        Shape.Circle(rad) => 3.0 * rad * rad,
        Shape.Rect(w, h) => w * h,
        Shape.Empty => 0.0
      };
      var first = match Shape.Rect(9.0, 1.0) {
        Shape.Rect(w, _) => w,
        _ => 0.0
      };
      return area + first;
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(EnumPayloadTest, MixedPayloadTypes) {
  auto value = executeString(R"(
    enum Value { Int(i64), Float(f64), Flag(bool), Pair(i32, u8) }

    function score(v: ref Value) i64 {
      return match v {
        Value.Int(i) => i,
        Value.Float(f) => 100,
        Value.Flag(b) => 200,
        Value.Pair(a, b) => a + b
      };
    }

    function main() i32 {
      var a = Value.Int(1000);
      var b = Value.Float(2.5);
      var c = Value.Flag(true);
      var d = Value.Pair(30, 12);
      return score(a) + score(b) + score(c) + score(d);
    }
  )");
  EXPECT_EQ(value, 1342);
}

TEST(EnumPayloadTest, RefParamAndReturnByValue) {
  auto value = executeString(R"(
    enum Shape { Circle(f64), Rect(f64, f64), Empty }

    function make(kind: i32) Shape {
      if (kind == 0) { return Shape.Circle(2.0); }
      if (kind == 1) { return Shape.Rect(3.0, 4.0); }
      return Shape.Empty;
    }

    function area(s: ref Shape) f64 {
      return match s {
        Shape.Circle(r) => 3.0 * r * r,
        Shape.Rect(w, h) => w * h,
        Shape.Empty => 0.0
      };
    }

    function main() i32 {
      var c = make(0);
      var r = make(1);
      var e = make(2);
      return area(c) + area(r) + area(e);
    }
  )");
  EXPECT_EQ(value, 24);
}

// Function return matched directly - regression for materializeStructReturn
TEST(EnumPayloadTest, MatchCallResultDirectly) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }

    function find(x: i32) Opt {
      if (x > 0) { return Opt.Some(x * 2); }
      return Opt.None;
    }

    function main() i32 {
      return match find(21) {
        Opt.Some(v) => v,
        Opt.None => -1
      };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumPayloadTest, CopyIndependence) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }

    function get(o: ref Opt) i32 {
      return match o {
        Opt.Some(v) => v,
        Opt.None => 0
      };
    }

    function main() i32 {
      var a = Opt.Some(10);
      var b = Opt.Some(get(a) + 5);
      // reassignment of a does not affect b
      a = Opt.Some(99);
      return get(a) + get(b);
    }
  )");
  EXPECT_EQ(value, 114);
}

TEST(EnumPayloadTest, EnumInClassField) {
  auto value = executeString(R"(
    enum State { Idle, Running(f32), Failed(i32) }

    class Task {
      var state: State;
      function init() {
        this.state = State.Idle;
      }
    }

    function code(t: ref Task) i32 {
      return match t.state {
        State.Idle => 1,
        State.Running(pct) => 2,
        State.Failed(c) => c
      };
    }

    function main() i32 {
      var t = Task();
      var idle = code(t);          // 1
      t.state = State.Failed(40);
      return idle + code(t);       // 1 + 40
    }
  )");
  EXPECT_EQ(value, 41);
}

TEST(EnumPayloadTest, ClassPayload) {
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init(x_: i32, y_: i32) {
        this.x = x_;
        this.y = y_;
      }
    }

    enum Hit { Miss, At(Point) }

    function main() i32 {
      var p = Point(30, 12);
      var h = Hit.At(p);
      return match h {
        Hit.At(pt) => pt.x + pt.y,
        Hit.Miss => 0
      };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumPayloadTest, NestedEnumPayload) {
  auto value = executeString(R"(
    enum Inner { A(i32), B }
    enum Outer { Wrap(Inner), Nothing }

    function main() i32 {
      var o = Outer.Wrap(Inner.A(42));
      return match o {
        Outer.Wrap(inner) => match inner {
          Inner.A(v) => v,
          Inner.B => -2
        },
        Outer.Nothing => -1
      };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumPayloadTest, EarlyReturnInArm) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }

    function pick(o: ref Opt) i32 {
      match o {
        Opt.Some(v) => { return v; },
        Opt.None => { return -1; }
      };
      return -99;
    }

    function main() i32 {
      var s = Opt.Some(5);
      var n = Opt.None;
      return pick(s) * (0 - pick(n));
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(EnumPayloadTest, BindingShadowsOuter) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }

    function main() i32 {
      var v = 100;
      var o = Opt.Some(42);
      var r = match o {
        Opt.Some(v) => v,
        Opt.None => 0
      };
      return r + v - 100;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumPayloadTest, CStyleEnumPayloadInVariant) {
  auto value = executeString(R"(
    enum Color { Red, Green, Blue }
    enum Paint { Solid(Color), Clear }

    function main() i32 {
      var p = Paint.Solid(Color.Blue);
      var c = match p {
        Paint.Solid(col) => col,
        Paint.Clear => Color.Red
      };
      if (c == Color.Blue) { return 1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// C-style enums keep the old behavior (i32, equality, expression patterns)
TEST(EnumPayloadTest, CStyleEnumRegression) {
  auto value = executeString(R"(
    enum Color { Red, Green, Blue }

    function main() i32 {
      var c: Color = Color.Green;
      if (c == Color.Green) {
        return match c {
          Color.Red => 1,
          Color.Green => 2,
          Color.Blue => 3
        };
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 2);
}

// ============================================================================
// Exhaustiveness
// ============================================================================

TEST(EnumPayloadTest, NonExhaustiveMatchIsError) {
  try {
    executeString(R"(
      enum Shape { Circle(f64), Rect(f64, f64), Empty }

      function main() i32 {
        var s = Shape.Empty;
        return match s {
          Shape.Circle(r) => 1,
          Shape.Empty => 2
        };
      }
    )");
    FAIL() << "expected non-exhaustive match error";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("Rect"), std::string::npos)
        << "error should name the missing variant, got: " << e.what();
  }
}

TEST(EnumPayloadTest, CStyleEnumExhaustivenessEnforced) {
  EXPECT_THROW(executeString(R"(
    enum Color { Red, Green, Blue }

    function main() i32 {
      var c: Color = Color.Red;
      return match c {
        Color.Red => 1,
        Color.Green => 2
      };
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, WildcardSatisfiesExhaustiveness) {
  auto value = executeString(R"(
    enum Shape { Circle(f64), Rect(f64, f64), Empty }

    function main() i32 {
      var s = Shape.Rect(1.0, 2.0);
      return match s {
        Shape.Circle(r) => 1,
        _ => 9
      };
    }
  )");
  EXPECT_EQ(value, 9);
}

// ============================================================================
// Error cases
// ============================================================================

TEST(EnumPayloadTest, PayloadVariantWithoutArgsIsError) {
  EXPECT_THROW(executeString(R"(
    enum Opt { Some(i32), None }
    function main() i32 {
      var x: Opt = Opt.Some;
      return 0;
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, UnitVariantWithArgsIsError) {
  EXPECT_THROW(executeString(R"(
    enum Opt { Some(i32), None }
    function main() i32 {
      var x = Opt.None(1);
      return 0;
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, WrongArityConstructionIsError) {
  EXPECT_THROW(executeString(R"(
    enum Shape { Rect(f64, f64) }
    function main() i32 {
      var s = Shape.Rect(1.0);
      return 0;
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, WrongArityPatternIsError) {
  EXPECT_THROW(executeString(R"(
    enum Shape { Rect(f64, f64), Empty }
    function main() i32 {
      var s = Shape.Empty;
      return match s {
        Shape.Rect(w) => 1,
        Shape.Empty => 2
      };
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, WrongPayloadTypeIsError) {
  EXPECT_THROW(executeString(R"(
    enum Opt { Some(i32), None }
    class Foo { var x: i32; function init() { this.x = 0; } }
    function main() i32 {
      var f = Foo();
      var x = Opt.Some(f);
      return 0;
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, EqualityOnPayloadEnumIsError) {
  EXPECT_THROW(executeString(R"(
    enum Opt { Some(i32), None }
    function main() i32 {
      var a = Opt.Some(1);
      var b = Opt.Some(1);
      if (a == b) { return 1; }
      return 0;
    }
  )"),
               std::exception);
}

// Payload enums pass by value with move semantics (like classes): the callee
// owns the argument and the caller's variable is moved-from afterwards.
TEST(EnumPayloadTest, ByValuePayloadEnumParamMoves) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }
    function get(o: Opt) i32 {
      return match o { Opt.Some(v) => v, Opt.None => -1 };
    }
    function main() i32 {
      var a = Opt.Some(41);
      return get(a) + 1;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(EnumPayloadTest, ByValuePayloadEnumParamUseAfterMoveIsError) {
  EXPECT_THROW(executeString(R"(
    enum Opt { Some(i32), None }
    function get(o: Opt) i32 { return 0; }
    function main() i32 {
      var a = Opt.Some(1);
      get(a);
      return get(a);
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, DestructuringOnNonEnumIsError) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
      var x = 5;
      return match x {
        Foo.Bar(v) => v,
        _ => 0
      };
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, NonVariantPatternOnEnumIsError) {
  EXPECT_THROW(executeString(R"(
    enum Opt { Some(i32), None }
    function main() i32 {
      var x = Opt.None;
      return match x {
        5 => 1,
        _ => 0
      };
    }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, RecursiveEnumIsError) {
  EXPECT_THROW(executeString(R"(
    enum Tree { Leaf(i32), Node(Tree) }
    function main() i32 { return 0; }
  )"),
               std::exception);
}

// A reference payload stores the referent's address: the variant borrows and
// owns nothing. This is what lets a container hand back Option<ref T> from a
// peek instead of a copy of an element it still owns.
TEST(EnumPayloadTest, RefPayloadBorrowsTheReferent) {
  auto value = executeString(R"(
    enum Holder { Of(ref i32), Empty }
    function main() i32 {
      var n: i32 = 41;
      var h = Holder.Of(n);
      n = n + 1;
      return match h {
        Holder.Of(r) => r,
        Holder.Empty => 0
      };
    }
  )");
  // r borrows n, so it sees the increment
  EXPECT_EQ(value, 42);
}

// Owning payloads (classes with deinit) are supported: see
// tests/test_enum_drops.cpp for the drop-glue behaviour.
TEST(EnumPayloadTest, DeinitClassPayloadCompiles) {
  auto value = executeString(R"(
    class Owner {
      var p: raw_ptr<i8>;
      function init() {
        var size: i64 = 8;
        this.p = unsafe { _malloc(size); };
      }
      function deinit() void {
        if (this.p != null) {
          unsafe { _free(this.p); };
          this.p = null;
        }
      }
    }
    enum Holder { Hold(Owner), Nothing }
    function main() i32 {
      var h = Holder.Hold(Owner());
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(EnumPayloadTest, EmptyPayloadParensIsError) {
  EXPECT_THROW(executeString(R"(
    enum Bad { Foo(), Bar }
    function main() i32 { return 0; }
  )"),
               std::exception);
}

TEST(EnumPayloadTest, GlobalPayloadEnumVarIsError) {
  EXPECT_THROW(executeString(R"(
    enum Opt { Some(i32), None }
    var g = Opt.Some(1);
    function main() i32 { return 0; }
  )"),
               std::exception);
}

// ============================================================================
// AOT compilation
// ============================================================================

TEST(EnumPayloadTest, AOTCompile) {
  EXPECT_NO_THROW(compileFile("tests/programs/enum_payload.sun"));
}

// ============================================================================
// Cross-module: payload enum exported through a .moon bundle
// ============================================================================

TEST(EnumPayloadTest, CrossModuleMoonBundle) {
  namespace fs = std::filesystem;
  initTestEnvironment();

  fs::path dir = fs::temp_directory_path() / "sun_enum_moon_test";
  fs::create_directories(dir);
  fs::path libSrc = dir / "shapes.sun";
  {
    std::ofstream out(libSrc);
    out << R"(
      public module shapes {
          public enum Shape { Circle(f64), Rect(f64, f64), Empty }

          public function area(s: ref Shape) f64 {
              return match s {
                  Shape.Circle(r) => 3.0 * r * r,
                  Shape.Rect(w, h) => w * h,
                  Shape.Empty => 0.0
              };
          }
      }
    )";
  }

  // Build the .moon bundle the same way `sun --emit-moon` does
  auto metadata = sun::extractMetadataFromFile(libSrc.string());
  ASSERT_TRUE(metadata.has_value());
  auto libDriver = Driver::createForAOT("moon_module");
  libDriver->compileFiles({libSrc.string()}, {});
  sun::MoonWriter writer;
  writer.addModule(libDriver->getModule(), *metadata);
  fs::path moonPath = dir / "shapes.moon";
  ASSERT_TRUE(writer.write(moonPath));

  // Import it: construct and destructure the payload enum across the boundary
  auto driver = Driver::createForJIT("moon_main");
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using shapes;

    function main() i32 {
        var c = Shape.Circle(2.0);
        var r = Shape.Rect(3.0, 4.0);
        return area(c) + area(r);
    }
  )");
  EXPECT_EQ(value, 24);
}
