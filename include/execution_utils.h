// Test utility functions that delegate to Driver
// This file provides convenient free functions for tests

#include <filesystem>
#include <iostream>
#include <string>

#include "driver.h"
#include "error.h"
#include "library_cache.h"
#include "sun_path.h"
#include "sun_value.h"

// Set SUN_PATH to cwd if not already set (for VS Code Test Explorer)
inline void initTestEnvironment() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    sun::SunPath::ensureSet();
    // Initialize library cache from environment
    sun::LibraryCache::instance().initFromEnvironment();
  });
}

// Execute and log SunError to stderr, then rethrow
inline sun::SunValue executeString(const std::string& source, int argc = 0,
                                   char** argv = nullptr) {
  initTestEnvironment();
  try {
    auto driver = Driver::createForJIT();
    driver->setDumpIR(true);  // Dump IR for debugging
    return driver->executeString(source, argc, argv);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

inline void compileFile(const std::string& filename) {
  try {
    initTestEnvironment();
    Driver::createForAOT()->compileFile(
        std::filesystem::absolute(filename).string());
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

inline void compileString(const std::string& source) {
  initTestEnvironment();
  try {
    Driver::createForAOT("test_compile")->compileString(source);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}