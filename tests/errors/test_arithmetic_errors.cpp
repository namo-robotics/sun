/** Verifies trapping operators and recoverable arithmetic results. */
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

/** Recognizes the signals emitted by LLVM traps on supported native targets. */
static bool arithmeticTrapped(int status) {
  return WIFSIGNALED(status) &&
         (WTERMSIG(status) == SIGILL || WTERMSIG(status) == SIGTRAP);
}

/** Selects one integer type and optimization setting for trap coverage. */
class Errors_ArithmeticTraps : public ::testing::TestWithParam<int> {};

/** Selects a signed integer type and optimization setting for overflow traps.
 */
class Errors_ArithmeticOverflow : public ::testing::TestWithParam<int> {};

/** Zero divisors trap for every integer width and assignment form. */
TEST_P(Errors_ArithmeticTraps, zero_divisors_trap) {
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
          auto driver = sun::driver::Driver::createForJIT("arithmetic_trap",
                                                          false, optimize);
          driver->executeString(source);
          _exit(0);
        },
        arithmeticTrapped, "");
  }
}

/** Signed overflow traps for division and remainder instead of producing
 * poison. */
TEST_P(Errors_ArithmeticOverflow, signed_overflow_traps) {
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
        arithmeticTrapped, "");
  }
}

INSTANTIATE_TEST_SUITE_P(IntegerWidths, Errors_ArithmeticTraps,
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
  EXPECT_EQ(executeString(R"(
    var drops = 0;
    /** Records resource cleanup. */
    class Owner {
      /** Acquires a fixture resource. */
      init() {}
      /** Releases the fixture resource. */
      deinit() { drops += 1; }
    }
    /** Returns a recoverable error while owning a local resource. */
    function divide(a: i32, b: i32) _Result<i32, ArithmeticError> {
      var owner = Owner();
      return _Result.Ok(try checked_div(a, b));
    }
    /** Propagates failure through a second resource-owning frame. */
    function outer() _Result<i32, ArithmeticError> {
      var owner = Owner();
      return _Result.Ok(try divide(1, 0));
    }
    /** Inspects the error after both owners have been dropped. */
    function main() i32 {
      return match outer() { _Result.Ok(value) => -1, _Result.Err(error) => drops };
    }
  )"),
            2);
}

/** Returned arithmetic errors expose owned messages through const interfaces.
 */
TEST(Errors_Arithmetic, messages_with_stdlib) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    /** Reads the message through a borrowed error interface. */
    function main() i32 {
      return match checked_div(1, 0) {
        _Result.Ok(value) => -1,
        (error: const ref IError) => {
          var message = error.message();
          if (message.equals_literal("integer division by zero")) { return 0; }
          return 1;
        }
      };
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

/** Builtin errors retain their message ABI across independently compiled
 * libraries. */
TEST(Errors_Arithmetic, library_result_without_stdlib_handled_with_stdlib) {
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
      public function divide(a: i32, b: i32) _Result<i32, ArithmeticError> { return checked_div(a, b); }
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
    /** Reads an owned message from a library's returned error. */
    function main() i32 {
      return match arithmetic_fixture.divide(1, 0) {
        _Result.Ok(value) => value,
        _Result.Err(error) => {
          var message = error.message();
          if (message.equals_literal("integer division by zero")) { return 0; }
          return 1;
        }
      };
    }
  )"),
            0);
}
