/** Shared helpers for compiler tests, linked into sun_tests. */

#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "driver/sun_value.h"
#include "moon_bundling/moon_import.h"
#include "support/error.h"

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {
/** Describes a library to preload into a test compilation. */
using sun::moon_bundling::MoonImport;
/** Reports a compiler or execution failure to the calling test. */
using sun::support::SunError;

/** Requires a compiler error containing the expected diagnostic text. */
#define EXPECT_SUN_ERROR_WITH_MESSAGE(stmt, expected_substr)            \
  do {                                                                  \
    try {                                                               \
      stmt;                                                             \
      FAIL() << "Expected sun::support::SunError to be thrown";         \
    } catch (const sun::support::SunError& e) {                         \
      EXPECT_NE(std::strstr(e.what(), expected_substr), nullptr)        \
          << "Expected error message to contain: \"" << expected_substr \
          << "\"\n"                                                     \
          << "Actual message: \"" << e.what() << "\"";                  \
    }                                                                   \
  } while (0)

/**
 * Set SUN_PATH to cwd if not already set (for VS Code Test Explorer)
 */
void initTestEnvironment();

/**
 * Get the stdlib.moon path for test preloading
 */
std::vector<MoonImport> getStdlibMoonImports();

/**
 * Execute and log SunError to stderr, then rethrow
 */
SunValue executeString(const std::string& source, int argc = 0,
                       char** argv = nullptr, bool includeStdlib = false);

/**
 * Execute with stdlib preloaded
 */
SunValue executeStringWithStdlib(const std::string& source, int argc = 0,
                                 char** argv = nullptr);

/**
 * Execute in test mode: tests kept, the runner main synthesized, stdlib
 * preloaded. Returns the runner's exit code (0 = every test passed).
 */
SunValue executeTestsWithStdlib(const std::string& source, int argc = 0,
                                char** argv = nullptr);

/**
 * Execute and dump all reachable IR (includes stdlib functions)
 */
SunValue executeStringWithReachableIR(const std::string& source, int argc = 0,
                                      char** argv = nullptr,
                                      bool includeStdlib = false);

/** Compiles a source file, optionally loading the standard library. */
void compileFile(const std::string& filename, bool includeStdlib = false);

/**
 * Compile with stdlib preloaded
 */
void compileFileWithStdlib(const std::string& filename);

/** Compiles source text, optionally loading the standard library. */
void compileString(const std::string& source, bool includeStdlib = false);

/**
 * Compile with stdlib preloaded
 */
void compileStringWithStdlib(const std::string& source);

/**
 * Compile multiple source files using the merged-AST model
 */
void compileFiles(const std::vector<std::string>& sourceFiles,
                  const std::vector<MoonImport>& moonImports = {});

/**
 * Execute multiple source files using the merged-AST model
 */
void executeFiles(const std::vector<std::string>& sourceFiles,
                  const std::vector<MoonImport>& moonImports = {});
/** Parses and analyzes source in a fresh driver, throwing reported errors.
 * Does not run borrow checking, code generation, linking, or execution.
 */
void analyzeString(const std::string& source, bool includeStdlib = false);

}  // namespace sun::driver
