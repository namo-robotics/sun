/** Exercises source exception lowering through the ordinary compiler and JIT. */
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "driver/driver.h"
#include "moon_bundling/moon_builder.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Runs each source scenario with both backend optimization settings. */
class Errors_SourceExceptions : public testing::TestWithParam<bool> {
 protected:
  /** Compiles and executes one source program using the real driver. */
  sun::driver::SunValue run(const std::string& source) {
    auto driver = sun::driver::Driver::createForJIT("exceptions", false, GetParam());
    return driver->executeString(source);
  }
};

/** A concrete throw transfers its scalar payload into a matching local catch. */
TEST_P(Errors_SourceExceptions, scalar_payload) {
  EXPECT_EQ(run(R"(
    /** Carries a numeric failure. */
    exception Failure { var code: i32; }
    /** Raises one bounded value. */
    function fail() void throws { var e: Failure = {code: 42}; throw e; }
    /** Reads the transferred payload. */
    function main() i32 {
      try { fail(); } catch (e: Failure) { return e.code; }
      return 0;
    }
  )"), 42);
}

/** A family catch retains the concrete type across another handled failure. */
TEST_P(Errors_SourceExceptions, family_rethrow) {
  EXPECT_EQ(run(R"(
    /** Groups related failures. */
    exception Failure {}
    /** Carries the original failure. */
    exception Original extends Failure { var code: i32; }
    /** Carries an independent failure. */
    exception Other { var code: i32; }
    /** Raises an original value. */
    function fail() void throws { var e: Original = {code: 42}; throw e; }
    /** Handles a different error before rethrowing the original. */
    function forward() void throws {
      try { fail(); } catch (e: Failure) {
        try { var b: Other = {code: 7}; throw b; } catch (b: Other) {}
        throw e;
      }
    }
    /** Matches the concrete type after the family rethrow. */
    function main() i32 {
      try { forward(); } catch (e: Original) { return e.code; }
      return 0;
    }
  )"), 42);
}

/** Exceptional cleanup may throw and handle a nested value without corrupting A. */
TEST_P(Errors_SourceExceptions, nested_cleanup) {
  EXPECT_EQ(run(R"(
    var dropped: i32 = 0;
    /** Carries a scalar error. */
    exception Failure { var code: i32; }
    /** Runs fallible work while being destroyed. */
    class Resource {
      /** Creates an empty resource. */
      init() {}
      /** Handles an internal failure before finishing cleanup. */
      deinit() {
        try { var b: Failure = {code: 20}; throw b; }
        catch (b: Failure) { dropped += b.code; }
      }
    }
    /** Leaves a resource live while propagating the original error. */
    function fail() void throws {
      var resource = Resource();
      var a: Failure = {code: 10};
      throw a;
    }
    /** Observes the original payload and completed cleanup. */
    function main() i32 {
      try { fail(); } catch (a: Failure) { return a.code + dropped; }
      return 0;
    }
  )"), 30);
}

/** A bare rethrow preserves edits made through the catch binding. */
TEST_P(Errors_SourceExceptions, mutated_bare_rethrow) {
  EXPECT_EQ(run(R"(
    /** Carries a mutable code. */
    exception Failure { var code: i32; }
    /** Changes a caught value before forwarding it. */
    function forward() void throws {
      try { var e: Failure = {code: 1}; throw e; }
      catch (e: Failure) { e.code = 42; throw; }
    }
    /** Observes the changed code. */
    function main() i32 {
      try { forward(); } catch (e: Failure) { return e.code; }
      return 0;
    }
  )"), 42);
}

/** Unrelated inner catches allow a matching outer catch to receive the value. */
TEST_P(Errors_SourceExceptions, outer_handler) {
  EXPECT_EQ(run(R"(
    /** Identifies the thrown family. */
    exception A {}
    /** Identifies an unrelated family. */
    exception B {}
    /** Selects the matching outer handler. */
    function main() i32 {
      try {
        try { var e: A = {}; throw e; } catch (e: B) { return 1; }
      } catch (e: A) { return 42; }
      return 0;
    }
  )"), 42);
}

/** Owning and borrowed payloads cannot bypass the bounded scalar contract. */
TEST_P(Errors_SourceExceptions, rejects_non_scalar_payload) {
  for (const auto* type : {"ptr<i32>", "raw_ptr<i32>", "ref i32", "array<i32, 33>"}) {
    SCOPED_TRACE(type);
    EXPECT_THROW(run(std::string("exception E { var value: ") + type +
                     "; } function main() i32 { return 0; }"), sun::support::SunError);
  }
}

/** Fixed arrays and enum fields retain their values across exception transport. */
TEST_P(Errors_SourceExceptions, bounded_array_and_enum_payload) {
  EXPECT_EQ(run(R"(
    /** Identifies a diagnostic category. */
    enum Category { Input, Output }
    /** Carries an inline diagnostic and its category. */
    exception Failure {
      var text: array<i32, 4> = [10, 20, 30, 40];
      var category: Category = Category.Output;
      /** Initializes the inline payload. */
      init() {}
      /** Reads a diagnostic byte through an ordinary method. */
      public const method first() i32 { return _convert<i32>(this.text[0]); }
    }
    /** Throws the bounded value without external storage. */
    function fail() void throws { throw Failure(); }
    /** Checks that both inline fields survived propagation. */
    function main() i32 {
      try { fail(); } catch (error: Failure) {
        if (error.category == Category.Output) { return error.first() + 32; }
      }
      return 0;
    }
  )"), 42);
}

/** Interface fields cannot introduce pointers into an exception payload. */
TEST_P(Errors_SourceExceptions, rejects_inherited_owning_field) {
  EXPECT_THROW(run(R"(
    /** Requires owning storage on implementations. */
    interface Storage { var value: ptr<i32>; }
    /** Attempts to inherit storage incompatible with bounded exceptions. */
    exception Failure implements Storage {}
    /** Forces declaration validation. */
    function main() i32 { return 0; }
  )"), sun::support::SunError);
}

/** Throwing functions require an explicit propagation or handling boundary. */
TEST_P(Errors_SourceExceptions, rejects_unhandled_effect) {
  EXPECT_THROW(run(R"(
    exception E {}
    function fail() void throws { var e: E = {}; throw e; }
    function main() i32 { fail(); return 0; }
  )"), sun::support::SunError);
}

/** Consuming a caught value prevents a later bare rethrow from duplicating it. */
TEST_P(Errors_SourceExceptions, rejects_rethrow_after_move) {
  EXPECT_THROW(run(R"(
    exception E {}
    function fail() void throws {
      try { var e: E = {}; throw e; }
      catch (e: E) { var moved = e; throw; }
    }
    function main() i32 { try { fail(); } catch {} return 0; }
  )"), sun::support::SunError);
}

/** Only initialized fields are destroyed when construction throws. */
TEST_P(Errors_SourceExceptions, partial_construction) {
  EXPECT_EQ(run(R"(
    var dropped: i32 = 0;
    /** Identifies construction failure. */
    exception Failure {}
    /** Counts completed field destruction. */
    class Field {
      /** Creates a field. */
      init() {}
      /** Records cleanup. */
      deinit() { dropped += 1; }
    }
    /** Fails between initialization of its two fields. */
    class Object {
      var first: Field;
      var second: Field;
      /** Initializes one field before failing. */
      init() throws { this.first = Field(); var e: Failure = {}; throw e; }
      /** Must not run for an incomplete object. */
      deinit() { dropped += 100; }
    }
    /** Observes exactly the one initialized field's cleanup. */
    function main() i32 {
      try { var value = Object(); } catch (e: Failure) { return dropped; }
      return 0;
    }
  )"), 1);
}

/** A cleanup failure must terminate before a stack propagation record expires. */
TEST_P(Errors_SourceExceptions, escaping_destructor_terminates) {
  EXPECT_DEATH(run(R"(
    /** Identifies the original failure. */
    exception A {}
    /** Identifies the escaping cleanup failure. */
    exception B {}
    /** Demonstrates a handler which does not cover its cleanup failure. */
    class Resource {
      /** Creates an empty resource. */
      init() {}
      /** Fails without a matching local handler. */
      deinit() { try { var b: B = {}; throw b; } catch (a: A) {} }
    }
    /** Starts an unwind while a resource is alive. */
    function fail() void throws { var r = Resource(); var a: A = {}; throw a; }
    /** Must not catch the destructor's escaped value. */
    function main() i32 { try { fail(); } catch {} return 0; }
  )"), "panic: Sun exception escaped");
}

/** Imported family metadata and concrete payloads survive an independent build. */
TEST_P(Errors_SourceExceptions, moon_family_rethrow) {
  const auto directory = std::filesystem::path("tmp") /
      ("source_exception_moon_" + std::to_string(getpid()) + (GetParam() ? "_opt" : "_plain"));
  std::filesystem::create_directories(directory);
  const auto source = directory / "library.sun";
  const auto bundle = directory / "library.moon";
  std::ofstream(source) << R"(
    /** Exports independently compiled exception identities. */
    public module failures {
      /** Groups related errors. */
      public exception Family {}
      /** Carries the concrete value. */
      public exception Concrete extends Family { var code: i32; }
      /** Raises a library-owned value. */
      public function fail() void throws { var e: Concrete = {code: 42}; throw e; }
    }
  )";
  sun::moon_bundling::MoonBuilder::build(source.string(), bundle);
  auto driver = sun::driver::Driver::createForJIT("moon_exceptions", false, GetParam());
  driver->setMoonImports({sun::moon_bundling::MoonImport(std::filesystem::absolute(bundle).string())});
  EXPECT_EQ(driver->executeString(R"(
    /** Rethrows an imported value through its family. */
    function forward() void throws {
      try { failures.fail(); } catch (e: failures.Family) { throw e; }
    }
    /** Matches the imported concrete identity after rethrow. */
    function main() i32 {
      try { forward(); } catch (e: failures.Concrete) { return e.code; }
      return 0;
    }
  )"), 42);
  std::filesystem::remove_all(directory);
}

/** A retained generic throwing function requires the ABI before instantiation. */
TEST_P(Errors_SourceExceptions, retained_generic_requires_exception_abi) {
  const auto directory = std::filesystem::path("tmp") /
      ("generic_exception_moon_" + std::to_string(getpid()) + (GetParam() ? "_opt" : "_plain"));
  std::filesystem::create_directories(directory);
  const auto source = directory / "library.sun";
  const auto bundle = directory / "library.moon";
  std::ofstream(source) << R"(
    /** Exports source that has no concrete LLVM function yet. */
    public module generic_errors {
      /** Declares a throwing effect without a concrete instantiation. */
      public function relay<T>(value: T) void throws {}
    }
  )";
  sun::moon_bundling::MoonBuilder::build(source.string(), bundle);
  auto reader = sun::moon_bundling::MoonReader::open(bundle);
  ASSERT_NE(reader, nullptr);
  bool requiresExceptionABI = false;
  for (const auto& rule : reader->getLibrary().bundle_contract().requirements()) {
    requiresExceptionABI |= rule.name() == "sun.exceptions.bounded";
  }
  EXPECT_TRUE(requiresExceptionABI);
  auto driver = sun::driver::Driver::createForJIT("generic_exceptions", false, GetParam());
  driver->setMoonImports({sun::moon_bundling::MoonImport(std::filesystem::absolute(bundle).string())});
  EXPECT_EQ(driver->executeString(R"(
    /** Carries a payload through retained generic code. */
    exception Failure { var code: i32; }
    /** Instantiates and handles the imported throwing function. */
    function main() i32 {
      try {
        var error: Failure = {code: 42};
        generic_errors.relay<Failure>(error);
        return 42;
      } catch (error: Failure) { return error.code; }
      return 0;
    }
  )"), 42);
  std::filesystem::remove_all(directory);
}

INSTANTIATE_TEST_SUITE_P(Optimization, Errors_SourceExceptions, testing::Bool());
