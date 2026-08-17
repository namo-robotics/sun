// tests/test_generic_enums.cpp - Generic enums (Stage 2): declaration,
// instantiation, type-argument inference, and match integration.

#include <gtest/gtest.h>

#include <fstream>

#include "execution_utils.h"
#include "metadata_extractor.h"
#include "moon/moon.h"
#include "moon_import.h"

// ============================================================================
// Option<T>
// ============================================================================

TEST(GenericEnumTest, OptionEndToEnd) {
  auto value = executeString(R"(
    enum Option<T> { Some(T), None }

    function find(x: i32) Option<i32> {
        if (x > 0) { return Option.Some(x * 2); }
        return Option.None;
    }

    function unwrap_or(o: ref Option<i32>, d: i32) i32 {
        return match o {
            Option.Some(v) => v,
            Option.None => d
        };
    }

    function main() i32 {
        var a = Option.Some(21);            // T inferred from the argument
        var b: Option<i32> = Option.None;   // T from the annotation
        var c = find(0);                    // return-position None
        var d = find(5);
        return unwrap_or(a, 0) + unwrap_or(b, 100) + unwrap_or(c, 200) +
               unwrap_or(d, 0);
    }
  )");
  EXPECT_EQ(value, 331);  // 21 + 100 + 200 + 10
}

TEST(GenericEnumTest, TwoSpecializationsCoexist) {
  auto value = executeString(R"(
    enum Option<T> { Some(T), None }

    function main() i32 {
        var i = Option.Some(40);
        var f = Option.Some(2.5);
        var fi = match f {
            Option.Some(v) => 2,      // f64 specialization matched
            Option.None => 0
        };
        var ii = match i {
            Option.Some(v) => v,
            Option.None => 0
        };
        return ii + fi;   // 40 + 2
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericEnumTest, ResultWithTwoParams) {
  auto value = executeString(R"(
    enum Result<T, E> { Ok(T), Err(E) }

    function checked_div(a: i32, b: i32) Result<i32, i32> {
        if (b == 0) { return Result.Err(-1); }
        return Result.Ok(a / b);
    }

    function get(r: ref Result<i32, i32>) i32 {
        return match r {
            Result.Ok(v) => v,
            Result.Err(e) => e
        };
    }

    function main() i32 {
        var good = checked_div(84, 2);
        var bad = checked_div(1, 0);
        return get(good) + get(bad);   // 42 + -1
    }
  )");
  EXPECT_EQ(value, 41);
}

// Expected type fills type params the arguments cannot determine
TEST(GenericEnumTest, ExpectedTypeFillsUnboundParams) {
  auto value = executeString(R"(
    enum Result<T, E> { Ok(T), Err(E) }

    function main() i32 {
        // Ok(7) binds T=i32; E comes from the annotation
        var r: Result<i32, bool> = Result.Ok(7);
        return match r {
            Result.Ok(v) => v,
            Result.Err(b) => 0
        };
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(GenericEnumTest, GenericEnumInClassField) {
  auto value = executeString(R"(
    enum Option<T> { Some(T), None }

    class Slot {
        var value: Option<i32>;
        function init() {
            this.value = Option.None;
        }
    }

    function main() i32 {
        var s = Slot();
        var before = match s.value {
            Option.Some(v) => v,
            Option.None => 1
        };
        s.value = Option.Some(41);
        var after = match s.value {
            Option.Some(v) => v,
            Option.None => 0
        };
        return before + after;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericEnumTest, NestedSpecializationPayload) {
  auto value = executeString(R"(
    enum Option<T> { Some(T), None }
    enum Wrap { Inner(Option<i32>), Nothing }

    function main() i32 {
        var w = Wrap.Inner(Option.Some(42));
        return match w {
            Wrap.Inner(opt) => match opt {
                Option.Some(v) => v,
                Option.None => -1
            },
            Wrap.Nothing => -2
        };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericEnumTest, PointerPayloadInference) {
  auto value = executeString(R"(
    enum Option<T> { Some(T), None }

    function main() i32 {
        var n: i64 = 4;
        var p = unsafe { _malloc(n); };
        var o = Option.Some(p);   // T = i8 via raw_ptr<i8> unification
        var r = match o {
            Option.Some(p2) => 1,
            Option.None => 0
        };
        unsafe { _free(p); };
        return r;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Errors
// ============================================================================

TEST(GenericEnumTest, BareNoneWithoutContextIsError) {
  EXPECT_THROW(executeString(R"(
    enum Option<T> { Some(T), None }
    function main() i32 {
      var x = Option.None;
      return 0;
    }
  )"),
               std::exception);
}

TEST(GenericEnumTest, WrongArityTypeArgsIsError) {
  EXPECT_THROW(executeString(R"(
    enum Option<T> { Some(T), None }
    function main() i32 {
      var x: Option<i32, i32> = Option.None;
      return 0;
    }
  )"),
               std::exception);
}

TEST(GenericEnumTest, ConflictingInferenceIsError) {
  EXPECT_THROW(executeString(R"(
    enum Pair<T> { Both(T, T) }
    function main() i32 {
      var x = Pair.Both(1.5, true);
      return 0;
    }
  )"),
               std::exception);
}

TEST(GenericEnumTest, SpecializationExhaustivenessEnforced) {
  EXPECT_THROW(executeString(R"(
    enum Option<T> { Some(T), None }
    function main() i32 {
      var x = Option.Some(1);
      return match x {
        Option.Some(v) => v
      };
    }
  )"),
               std::exception);
}

TEST(GenericEnumTest, DeinitPayloadTypeArgIsError) {
  EXPECT_THROW(executeString(R"(
    class Owner {
      var p: raw_ptr<i8>;
      function init() { this.p = unsafe { _malloc(8); }; }
      function deinit() void { unsafe { _free(this.p); }; }
    }
    enum Option<T> { Some(T), None }
    function main() i32 {
      var o = Owner();
      var x = Option.Some(o);
      return 0;
    }
  )"),
               std::exception);
}

// ============================================================================
// Cross-module: generic enum exported through a .moon bundle
// ============================================================================

TEST(GenericEnumTest, CrossModuleMoonBundle) {
  namespace fs = std::filesystem;
  initTestEnvironment();

  fs::path dir = fs::temp_directory_path() / "sun_generic_enum_moon_test";
  fs::create_directories(dir);
  fs::path libSrc = dir / "optlib.sun";
  {
    std::ofstream out(libSrc);
    out << R"(
      public module optlib {
          public enum Option<T> { Some(T), None }

          public function pick(x: i32) Option<i32> {
              if (x > 0) { return Option.Some(x); }
              return Option.None;
          }
      }
    )";
  }

  auto metadata = sun::extractMetadataFromFile(libSrc.string());
  ASSERT_TRUE(metadata.has_value());
  auto libDriver = Driver::createForAOT("moon_module");
  libDriver->compileFiles({libSrc.string()}, {});
  sun::SunLibWriter writer;
  writer.addModule(libDriver->getModule(), *metadata);
  fs::path moonPath = dir / "optlib.moon";
  ASSERT_TRUE(writer.write(moonPath));

  // Instantiate the imported generic enum with a NEW type argument (f64) and
  // use the library's own i32 specialization
  auto driver = Driver::createForJIT("moon_main");
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using optlib;

    function main() i32 {
        var a = pick(30);
        var b = Option.Some(2.5);
        var ai = match a {
            Option.Some(v) => v,
            Option.None => 0
        };
        var bi = match b {
            Option.Some(v) => 12,
            Option.None => 0
        };
        return ai + bi;
    }
  )");
  EXPECT_EQ(value, 42);
}
