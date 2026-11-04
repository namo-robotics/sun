/** Implements shared test operations without exposing driver internals. */
#include "driver/execution_utils.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>

#include "driver/driver.h"
#include "moon_bundling/library_cache.h"
#include "support/sun_path.h"

/** Runs each test operation with independent compiler state. */
namespace sun::driver {
void initTestEnvironment() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    sun::support::SunPath::ensureSet();
    // Initialize library cache from environment
    sun::moon_bundling::LibraryCache::instance().initFromEnvironment();
  });
}

std::vector<MoonImport> getStdlibMoonImports() {
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

SunValue executeString(const std::string& source, int argc, char** argv,
                       bool includeStdlib) {
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

SunValue executeStringWithStdlib(const std::string& source, int argc,
                                 char** argv) {
  return executeString(source, argc, argv, true);
}

SunValue executeTestsWithStdlib(const std::string& source, int argc,
                                char** argv) {
  initTestEnvironment();
  try {
    auto driver = Driver::createForJIT();
    driver->setTestHandling(Driver::TestHandling::Compile);
    driver->setMoonImports(getStdlibMoonImports());
    return driver->executeString(source, argc, argv);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

SunValue executeStringWithReachableIR(const std::string& source, int argc,
                                      char** argv, bool includeStdlib) {
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

void compileFile(const std::string& filename, bool includeStdlib) {
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

void compileFileWithStdlib(const std::string& filename) {
  compileFile(filename, true);
}

void compileString(const std::string& source, bool includeStdlib) {
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

void compileStringWithStdlib(const std::string& source) {
  compileString(source, true);
}

void compileFiles(const std::vector<std::string>& sourceFiles,
                  const std::vector<MoonImport>& moonImports) {
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

void executeFiles(const std::vector<std::string>& sourceFiles,
                  const std::vector<MoonImport>& moonImports) {
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

void analyzeString(const std::string& source, bool includeStdlib) {
  initTestEnvironment();
  auto driver = Driver::createForJIT();
  if (includeStdlib) {
    driver->setMoonImports(getStdlibMoonImports());
  }
  auto program = driver->analyzeString(source);
  if (program.error) {
    throw *program.error;
  }
  if (!program.ast) {
    sun::support::logAndThrowError("Analysis did not produce a syntax tree");
  }
}
}  // namespace sun::driver
