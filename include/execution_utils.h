// Test utility functions that delegate to Driver
// This file provides convenient free functions for tests

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "driver.h"
#include "error.h"
#include "library_cache.h"
#include "moon_import.h"
#include "sun_path.h"
#include "sun_value.h"

// Helper macro for testing SunError with message content
#define EXPECT_SUN_ERROR_WITH_MESSAGE(stmt, expected_substr)            \
  do {                                                                  \
    try {                                                               \
      stmt;                                                             \
      FAIL() << "Expected SunError to be thrown";                       \
    } catch (const SunError& e) {                                       \
      EXPECT_NE(std::strstr(e.what(), expected_substr), nullptr)        \
          << "Expected error message to contain: \"" << expected_substr \
          << "\"\n"                                                     \
          << "Actual message: \"" << e.what() << "\"";                  \
    }                                                                   \
  } while (0)

// Set SUN_PATH to cwd if not already set (for VS Code Test Explorer)
inline void initTestEnvironment() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    sun::SunPath::ensureSet();
    // Initialize library cache from environment
    sun::LibraryCache::instance().initFromEnvironment();
  });
}

// Get the stdlib.moon path for test preloading
inline std::vector<sun::MoonImport> getStdlibMoonImports() {
  auto stdlibPath = std::filesystem::path("build/stdlib.moon");
  if (!std::filesystem::exists(stdlibPath)) {
    // Try absolute from SUN_PATH
    const char* sunPath = std::getenv("SUN_PATH");
    if (sunPath) {
      stdlibPath = std::filesystem::path(sunPath) / "build" / "stdlib.moon";
    }
  }
  if (std::filesystem::exists(stdlibPath)) {
    return {{std::filesystem::absolute(stdlibPath).string(), {}}};
  }
  return {};
}

// Execute and log SunError to stderr, then rethrow
inline sun::SunValue executeString(const std::string& source, int argc = 0,
                                   char** argv = nullptr,
                                   bool includeStdlib = false) {
  initTestEnvironment();
  try {
    auto driver = Driver::createForJIT();
    driver->setDumpReachable(true);  // Dump IR for debugging
    if (includeStdlib) {
      driver->setMoonImports(getStdlibMoonImports());
    }
    return driver->executeString(source, argc, argv);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

// Execute with stdlib preloaded
inline sun::SunValue executeStringWithStdlib(const std::string& source,
                                             int argc = 0,
                                             char** argv = nullptr) {
  return executeString(source, argc, argv, true);
}

// Execute and dump all reachable IR (includes stdlib functions)
inline sun::SunValue executeStringWithReachableIR(const std::string& source,
                                                  int argc = 0,
                                                  char** argv = nullptr,
                                                  bool includeStdlib = false) {
  initTestEnvironment();
  try {
    auto driver = Driver::createForJIT();
    driver->setDumpIR(true);
    driver->setDumpReachable(true);  // Include stdlib functions
    if (includeStdlib) {
      driver->setMoonImports(getStdlibMoonImports());
    }
    return driver->executeString(source, argc, argv);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

inline void compileFile(const std::string& filename,
                        bool includeStdlib = false) {
  try {
    initTestEnvironment();
    auto driver = Driver::createForAOT();
    if (includeStdlib) {
      driver->setMoonImports(getStdlibMoonImports());
    }
    driver->compileFile(std::filesystem::absolute(filename).string());
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

// Compile with stdlib preloaded
inline void compileFileWithStdlib(const std::string& filename) {
  compileFile(filename, true);
}

inline void compileString(const std::string& source,
                          bool includeStdlib = false) {
  initTestEnvironment();
  try {
    auto driver = Driver::createForAOT("test_compile");
    if (includeStdlib) {
      driver->setMoonImports(getStdlibMoonImports());
    }
    driver->compileString(source);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

// Compile multiple source files using the merged-AST model
inline void compileFiles(const std::vector<std::string>& sourceFiles,
                         const std::vector<sun::MoonImport>& moonImports = {}) {
  initTestEnvironment();
  try {
    std::vector<std::string> absolutePaths;
    for (const auto& file : sourceFiles) {
      absolutePaths.push_back(std::filesystem::absolute(file).string());
    }
    Driver::createForAOT("test_merged")
        ->compileFiles(absolutePaths, moonImports);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

// Execute multiple source files using the merged-AST model
inline void executeFiles(const std::vector<std::string>& sourceFiles,
                         const std::vector<sun::MoonImport>& moonImports = {}) {
  initTestEnvironment();
  try {
    std::vector<std::string> absolutePaths;
    for (const auto& file : sourceFiles) {
      absolutePaths.push_back(std::filesystem::absolute(file).string());
    }
    Driver::createForJIT("test_merged")
        ->executeFiles(absolutePaths, moonImports);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}