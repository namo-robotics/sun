/** Verifies trapping operators and recoverable arithmetic exceptions. */
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "driver/driver.h"
#include "driver/execution_utils.h"

using sun::driver::executeString;

/** Prevents expected child crashes from producing dumps or crash-report jobs.
 */
static void disableExpectedCoreDump() {
  const rlimit limit{0, 0};
  if (setrlimit(RLIMIT_CORE, &limit) != 0) {
    _exit(125);
  }
#ifdef __linux__
  // Piped Linux core handlers can ignore RLIMIT_CORE; disable dumping as well.
  if (prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0) {
    _exit(125);
  }
#endif
}

/** Selects one integer type and optimization setting for panic coverage. */
class Errors_ArithmeticPanics : public ::testing::TestWithParam<int> {};

/** Selects a signed integer type and optimization setting for overflow panics.
 */
class Errors_ArithmeticOverflow : public ::testing::TestWithParam<int> {};

/** Zero divisors abort for every integer width and assignment form. */
TEST_P(Errors_ArithmeticPanics, zero_divisors_abort) {
  const bool optimize = GetParam() >= 8;
  const std::string type = std::array{
      "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"}[GetParam() % 8];
  for (const std::string op : {"/", "%", "/=", "%="}) {
    std::string body =
        op.size() == 2 ? "a " + op + " b; return a;" : "return a " + op + " b;";
    std::string source =
        "/** Performs ordinary arithmetic. */\nfunction apply(a: " + type +
        ", b: " + type + ") " + type + " { " + body +
        " }\n"
        "/** Supplies a runtime zero divisor. */\nfunction main() i32 { "
        "return _convert<i32>(apply(7, 0)); }";
    SCOPED_TRACE(type + op);
    EXPECT_EXIT(
        {
          disableExpectedCoreDump();
          auto driver = sun::driver::Driver::createForJIT("arithmetic_panic",
                                                          false, optimize);
          driver->executeString(source);
          _exit(0);
        },
        ::testing::KilledBySignal(SIGABRT), "panic: integer division");
  }
}

/** Signed overflow panics for division and remainder instead of producing
 * poison. */
TEST_P(Errors_ArithmeticOverflow, signed_overflow_aborts) {
  const bool optimize = GetParam() >= 4;
  const std::string type =
      std::array{"i8", "i16", "i32", "i64"}[GetParam() % 4];
  int width = std::stoi(type.substr(1));
  std::string minimum = width == 64
                            ? "-9223372036854775807 - 1"
                            : std::to_string(-(int64_t{1} << (width - 1)));
  for (const std::string op : {"/", "%", "/=", "%="}) {
    std::string body =
        op.size() == 2 ? "a " + op + " b; return a;" : "return a " + op + " b;";
    std::string source =
        "/** Performs signed arithmetic. */\nfunction apply(a: " + type +
        ", b: " + type + ") " + type + " { " + body +
        " }\n"
        "/** Supplies overflowing operands. */\nfunction main() i32 { "
        "return _convert<i32>(apply(" +
        minimum + ", -1)); }";
    SCOPED_TRACE(type + op);
    EXPECT_EXIT(
        {
          disableExpectedCoreDump();
          auto driver = sun::driver::Driver::createForJIT("arithmetic_overflow",
                                                          false, optimize);
          driver->executeString(source);
          _exit(0);
        },
        ::testing::KilledBySignal(SIGABRT), "panic: integer division");
  }
}

INSTANTIATE_TEST_SUITE_P(IntegerWidths, Errors_ArithmeticPanics,
                         ::testing::Range(0, 16));
INSTANTIATE_TEST_SUITE_P(IntegerWidths, Errors_ArithmeticOverflow,
                         ::testing::Range(0, 8));

/** Valid variable divisors need no error-returning signature. */
TEST(Errors_Arithmetic, ordinary_operations_need_no_error_contract) {
  for (const std::string op : {"/", "%", "/=", "%="}) {
    std::string body =
        op.size() == 2 ? "a " + op + " b; return a;" : "return a " + op + " b;";
    EXPECT_EQ(executeString("/** Divides or computes remainder. */\nfunction "
                            "calculate(a: i32, b: i32) i32 { " +
                            body +
                            " }\n"
                            "/** Uses valid runtime inputs. */\nfunction "
                            "main() i32 { return calculate(7, 2); }"),
              op[0] == '/' ? 3 : 1);
  }
}

/** Checked arithmetic propagates failure and drops owners in abandoned frames.
 */
TEST(Errors_Arithmetic, propagation_drops_live_values) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(

    var drops = 0;
    /** Records resource cleanup. */
    class Owner {
      /** Acquires a fixture resource. */
      init() {}
      /** Releases the fixture resource. */
      deinit() { drops += 1; }
    }
    /** Returns a recoverable error while owning a local resource. */
    function divide(a: i32, b: i32) i32 throws {
      var owner = Owner();
      return std.checked_div(a, b);
    }
    /** Propagates failure through a second resource-owning frame. */
    function outer() i32 throws {
      var owner = Owner();
      return  divide(1, 0);
    }
    /** Inspects the error after both owners have been dropped. */
    function main() i32 {
      try { outer(); } catch (error: std.ArithmeticError) { return drops; }
      return -1;
    }
  )"),
            2);
}

/** Standard arithmetic errors return owned diagnostic text.
 */
TEST(Errors_Arithmetic, messages_with_stdlib) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    /** Reads the concrete error and explicitly owns its diagnostic. */
    function main() i32 throws {
      try { std.checked_div(1, 0); }
      catch (error: std.ArithmeticError) {
        var message = error.message();
        return message.equals_literal("integer division by zero") ? 0 : 1;
      }
      return -1;
    }
  )"),
            0);
}

/** A successful operation keeps resources alive until normal scope exit. */
TEST(Errors_Arithmetic, success_keeps_owners_alive) {
  EXPECT_EQ(executeString(R"(
    var drops = 0;
    /** Records normal cleanup. */
    class Owner {
      /** Acquires the resource. */
      init() {}
      /** Releases the resource. */
      deinit() { drops += 1; }
    }
    /** Keeps its owner alive while using the quotient. */
    function divide(a: i32, b: i32) i32 {
      var owner = Owner();
      var result = a / b;
      if (drops != 0) { return -1; }
      return result;
    }
    /** Verifies that cleanup happens once after success. */
    function main() i32 { if (divide(8, 2) != 4) { return -1; } return drops; }
  )"),
            1);
}

/** Primitive arithmetic failures cross libraries without a stdlib dependency. */
TEST(Errors_Arithmetic, library_exception_without_stdlib_handled_with_stdlib) {
  const auto directory =
      std::filesystem::path("tmp") /
      ("arithmetic_error_library_" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto source = directory / "library.sun";
  const auto library = directory / "library.moon";
  std::ofstream(source) << R"(
    /** Exports integer division without depending on the standard library. */
    public module arithmetic_fixture {
      /** Propagates arithmetic errors across a library boundary. */
      public function divide(a: i32, b: i32) i32 throws {
        if (b == 0) { throw Failure(); }
        return a / b;
      }
      /** Carries a bounded code without importing standard-library types. */
      public exception Failure { public var code: i32 = 4; }
    }
  )";
  // These paths contain only the fixed prefix, the process id, and filenames.
  const std::string command = "build/sun --emit-moon -o " + library.string() +
                              " " + source.string() + " > " +
                              (directory / "build.log").string() + " 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0);
  auto driver = sun::driver::Driver::createForJIT("arithmetic_import");
  auto imports = sun::driver::getStdlibMoonImports();
  imports.push_back({std::filesystem::absolute(library).string(), {}});
  driver->setMoonImports(std::move(imports));
  EXPECT_EQ(driver->executeString(R"(
    /** Converts a library's primitive failure into a standard diagnostic. */
    function main() i32 throws {
      try { arithmetic_fixture.divide(1, 0); }
      catch (error: arithmetic_fixture.Failure) {
        var message = std.ArithmeticError(error.code).message();
        return message.equals_literal("integer division by zero") ? 0 : 1;
      }
      return -1;
    }
  )"),
            0);
}

/** Standard checked arithmetic uses interface matching and preserves literal coercion. */
TEST(Errors_Arithmetic, stdlib_checked_promotion_and_interface) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Checks the public wrappers without converting their operands. */
    function main() i32 throws {
      var small: i8 = 12;
      var wide: i64 = 3;
      var narrow: i8 = checked_div(small, 2);
      var reverse: i8 = std.checked_rem(13, small);
      var promoted: i64 = std.checked_div(small, wide);
      var widened_literal: i32 = std.checked_div(small, 256);
      var total = _convert<i32>(narrow);
      total += _convert<i32>(reverse);
      total += _convert<i32>(promoted);
      total += _convert<i32>(widened_literal);
      try { std.checked_div(small, 0); }
      catch (error: ArithmeticError) {
        var message = error.message();
        if (not message.equals_literal("integer division by zero")) { return -2; }
        return total + error.code();
      }
      return -1;
    }
  )"), 15);
}

/** Explicit panics abort without releasing live owners. */
TEST(Errors_Arithmetic, explicit_panic_skips_cleanup) {
  EXPECT_EXIT({
    disableExpectedCoreDump();
    executeString(R"(
      /** Makes cleanup observable on stderr. */
      class Owner {
        /** Creates a resource for the panic test. */
        init() {}
        /** Must not execute during an abort. */
        deinit() { _panic("unexpected cleanup"); }
      }
      /** Aborts while a local resource is still alive. */
      function main() i32 {
        var owner = Owner();
        _panic("requested failure");
        return 0;
      }
    )");
    _exit(0);
  }, ::testing::KilledBySignal(SIGABRT), "^panic: requested failure\n$");
}

/** The public panic wrapper uses the same abort path as compiler checks. */
TEST(Errors_Arithmetic, stdlib_panic_aborts) {
  EXPECT_EXIT({
    disableExpectedCoreDump();
    sun::driver::executeStringWithStdlib(R"(
      /** Reports an explicit unrecoverable failure. */
      function main() i32 { std.panic("public failure"); return 0; }
    )");
    _exit(0);
  }, ::testing::KilledBySignal(SIGABRT), "^panic: public failure\n$");
}

/** Public error identity and interface methods survive a compiled-library boundary. */
TEST(Errors_Arithmetic, library_stdlib_error_implements_interface) {
  const auto directory = std::filesystem::path("tmp") /
      ("stdlib_arithmetic_library_" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto source = directory / "library.sun";
  const auto library = directory / "library.moon";
  std::ofstream(source) << R"(
    /** Exports the standard checked arithmetic API through a library. */
    public module arithmetic_fixture {
      /** Returns an ordinary standard-library error on failure. */
      public function divide(a: i32, b: i32) i32 throws {
        return std.checked_div(a, b);
      }
    }
  )";
  const std::string command = "build/sun --moon build/stdlib.moon --emit-moon -o " +
      library.string() + " " + source.string() + " > " +
      (directory / "build.log").string() + " 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0);
  auto driver = sun::driver::Driver::createForJIT("stdlib_arithmetic_import");
  auto imports = sun::driver::getStdlibMoonImports();
  imports.push_back({std::filesystem::absolute(library).string(), {}});
  driver->setMoonImports(std::move(imports));
  EXPECT_EQ(driver->executeString(R"(
    /** Uses the interface without depending on a builtin error representation. */
    function main() i32 throws {
      try { arithmetic_fixture.divide(1, 0); }
      catch (error: std.ArithmeticError) {
        var view: const ref std.IError = error;
        var message = view.message();
        if (not message.equals_literal("integer division by zero")) { return -2; }
        return view.code();
      }
      return -1;
    }
  )"), 4);
}

/** Native arithmetic checks emit diagnostics and abort just like JIT checks. */
TEST(Errors_Arithmetic, native_panic_diagnostic) {
  const auto directory = std::filesystem::path("tmp") /
      ("native_arithmetic_panic_" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto source = directory / "main.sun";
  const auto binary = directory / "panic";
  std::ofstream(source) << R"(
    /** Keeps the divisor a runtime operand in unoptimized code. */
    function divide(value: i32, divisor: i32) i32 { return value / divisor; }
    /** Exercises the native diagnostic abort path. */
    function main() i32 { return divide(42, 0); }
  )";
  const std::string command = "build/sun -O0 -c -o " + binary.string() +
      " " + source.string() + " > " + (directory / "build.log").string() + " 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0);
  EXPECT_EXIT({
    disableExpectedCoreDump();
    execl(binary.c_str(), binary.c_str(), nullptr);
    _exit(125);
  }, ::testing::KilledBySignal(SIGABRT), "^panic: integer division by zero\n$");
}

/** Integer promotion retains mixed signedness and operand-order rules. */
TEST(Errors_Arithmetic, stdlib_mixed_signedness) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    /** Verifies both width promotion and equal-width signedness. */
    function main() i32 throws {
      var narrow: i16 = 12;
      var wide: u32 = 3;
      var signed: i32 = 12;
      var first: u32 = std.checked_div(narrow, wide);
      var second: i32 = std.checked_div(signed, wide);
      var third: u32 = std.checked_rem(wide, signed);
      var total = _convert<i32>(first);
      total += second;
      total += _convert<i32>(third);
      return total;
    }
  )"), 11);
}

/** Promotion queries and primitive checked operations reject floating-point inputs. */
TEST(Errors_Arithmetic, rejects_non_integer_checked_operands) {
  EXPECT_ANY_THROW(sun::driver::analyzeString(R"(
    /** Attempts to use a floating-point type in integer promotion. */
    function invalid(value: f64) _promoted_integer<f64, i32> { return value; }
  )"));
  EXPECT_ANY_THROW(sun::driver::analyzeString(R"(
    /** Attempts to use floating-point operands with checked integer division. */
    function main() i32 { var result = _division_overflows(1.0, 0.0); return 0; }
  )"));
}
