/** Tests checked integer arithmetic and the builtin ArithmeticError type. */
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "driver/execution_utils.h"

using sun::driver::executeString;

/** Zero divisors throw concrete errors for every width and assignment form. */
TEST(Errors_Arithmetic, zero_divisors) {
  for (bool optimize : {false, true}) {
    for (const std::string type :
         {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"}) {
      for (const std::string op : {"/", "%"}) {
        for (bool compound : {false, true}) {
          std::string body = compound ? "a " + op + "= b; return a;"
                                      : "return a " + op + " b;";
          std::string source =
              "/** Applies an operation that may throw. */\n"
              "function apply(a: " +
              type + ", b: " + type + ") " + type + " throws IError { " + body +
              " }\n"
              "/** Checks the concrete builtin error. */\n"
              "function main() i32 { try { apply(7, 0); return -1; }"
              "catch (e: ref ArithmeticError) { return e.code(); } }";
          SCOPED_TRACE(type + op + (compound ? "=" : ""));
          auto driver = sun::driver::Driver::createForJIT("arithmetic_errors",
                                                          false, optimize);
          EXPECT_EQ(driver->executeString(source), 4);
        }
      }
    }
  }
}

/** Signed division and remainder overflow throw instead of producing poison. */
TEST(Errors_Arithmetic, signed_overflow) {
  for (bool optimize : {false, true}) {
    for (const std::string type : {"i8", "i16", "i32", "i64"}) {
      int width = std::stoi(type.substr(1));
      std::string minimum = width == 64
                                ? "-9223372036854775807 - 1"
                                : std::to_string(-(int64_t{1} << (width - 1)));
      for (const std::string op : {"/", "%", "/=", "%="}) {
        std::string body = op.size() == 2 ? "a " + op + " b; return a;"
                                          : "return a " + op + " b;";
        std::string source =
            "/** Applies signed division or remainder. */\n"
            "function apply(a: " +
            type + ", b: " + type + ") " + type + " throws IError { " + body +
            " }\n"
            "/** Catches through the common error interface. */\n"
            "function main() i32 { try { apply(" +
            minimum +
            ", -1); return -1; }"
            "catch (e: ref IError) { return e.code(); } }";
        auto driver = sun::driver::Driver::createForJIT("arithmetic_overflow",
                                                        false, optimize);
        EXPECT_EQ(driver->executeString(source), 5);
      }
    }
  }
}

/** A local handler permits division without changing the function signature. */
TEST(Errors_Arithmetic, local_handler_and_user_example) {
  EXPECT_EQ(executeString(R"(
    /** Propagates a division error to its caller. */
    function divide(a: i32, b: i32) i32 throws IError { return a / b; }
    /** Recovers from the failure. */
    function main() i32 {
      try { return divide(1, 0); } catch (e: ref IError) { return 0; }
    }
  )"),
            0);
  EXPECT_EQ(executeString(R"(
    /** Handles arithmetic directly within its own body. */
    function main() i32 {
      var divisor: i32 = 0;
      try { return 1 / divisor; } catch (e: ref ArithmeticError) { return e.code(); }
    }
  )"),
            4);
}

/** Potential arithmetic failures require the same handling as throwing calls.
 */
TEST(Errors_Arithmetic, unhandled_operations_are_rejected) {
  for (const std::string op : {"/", "%", "/=", "%="}) {
    std::string body =
        op.size() == 2 ? "a " + op + " b; return a;" : "return a " + op + " b;";
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        executeString("/** Intentionally lacks an error contract. */\n"
                      "function divide(a: i32, b: i32) i32 { " +
                      body +
                      " }\n"
                      "/** Provides an entrypoint. */\nfunction main() i32 { "
                      "return divide(7, 2); }"),
        "may throw ArithmeticError");
  }
}

/** Constant divisors, including widened unsigned ones, need no error contract.
 */
TEST(Errors_Arithmetic, statically_safe_division) {
  EXPECT_EQ(executeString(R"(
    const divisor: i32 = 2;
    /** Divides by a known nonzero divisor that cannot overflow. */
    function divide(a: i32) i32 { return a / divisor; }
    /** Checks constants and a widened unsigned divisor. */
    function main() i32 {
      const byte: u8 = 255;
      return divide(84) + (7 / -1) + (-2147483648 / byte);
    }
  )"),
            42 - 7 + (-2147483647 - 1) / 255);
}

/** Errors clean up both the failing frame and the caller's protected scope. */
TEST(Errors_Arithmetic, unwinding_drops_live_values) {
  EXPECT_EQ(executeString(R"(
    var drops: i32 = 0;
    /** Records destruction during unwinding. */
    class Owner {
      /** Creates a tracked owner. */
      init() {}
      /** Records cleanup. */
      deinit() { drops += 1; }
    }
    /** Fails while owning a local value. */
    function divide(a: i32, b: i32) i32 throws IError {
      var owner = Owner();
      return a / b;
    }
    /** Catches only after all abandoned owners have been dropped. */
    function main() i32 {
      try {
        var owner = Owner();
        return divide(1, 0);
      } catch (e: ref ArithmeticError) { return drops; }
    }
  )"),
            2);
}

/** Concrete and interface catches expose owned messages with the stdlib loaded.
 */
TEST(Errors_Arithmetic, messages_with_stdlib) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    /** Tests the arithmetic error's standard error interface. */
    function main() i32 {
      try { var zero: i32 = 0; return 1 / zero; }
      catch (e: ref IError) {
        var message = e.message();
        if (message.equals_literal("integer division by zero")) { return 0; }
        return 1;
      }
    }
  )"),
            0);
}

/** A successful division keeps owners alive until normal scope exit. */
TEST(Errors_Arithmetic, success_keeps_owners_alive) {
  EXPECT_EQ(executeString(R"(
    var drops: i32 = 0;
    /** Tracks destruction around arithmetic guards. */
    class Owner {
      /** Creates a tracked owner. */
      init() {}
      /** Records destruction. */
      deinit() { drops += 1; }
    }
    /** Performs valid arithmetic while keeping a local owner alive. */
    function divide(a: i32, b: i32) i32 throws IError {
      var owner = Owner();
      var result = a / b;
      if (drops != 0) { return -1; }
      return result;
    }
    /** Verifies normal cleanup still happens exactly once. */
    function main() i32 throws IError {
      if (divide(8, 2) != 4) { return -1; }
      return drops;
    }
  )"),
            1);
}

/** A caught builtin error can be rethrown and matched by an outer handler. */
TEST(Errors_Arithmetic, nested_rethrow) {
  EXPECT_EQ(executeString(R"(
    /** Throws a caught arithmetic error without losing its concrete identity. */
    function divide(a: i32, b: i32) i32 throws IError {
      try { return a / b; }
      catch (e: ref ArithmeticError) { throw e; }
    }
    /** Matches the rethrown error. */
    function main() i32 {
      try { return divide(1, 0); }
      catch (e: ref ArithmeticError) { return e.code(); }
    }
  )"),
            4);
}

/** Builtin errors retain their message ABI across independently compiled
 * libraries. */
TEST(Errors_Arithmetic, library_without_stdlib_caught_with_stdlib) {
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
      public function divide(a: i32, b: i32) i32 throws IError { return a / b; }
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
    /** Catches a library's builtin error through the local error interface. */
    function main() i32 {
      try { return arithmetic_fixture.divide(1, 0); }
      catch (e: ref IError) {
        var message = e.message();
        if (message.equals_literal("integer division by zero")) { return 0; }
        return 1;
      }
    }
  )"),
            0);
}
