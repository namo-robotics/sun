// Test utility functions that delegate to Driver
// This file provides convenient free functions for tests

#include <filesystem>
#include <iostream>
#include <string>

#include "driver.h"
#include "error.h"
#include "library_cache.h"
#include "sun_value.h"

// Set SUN_PATH to cwd if not already set (for VS Code Test Explorer)
inline void ensureSunPathSet() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    if (!std::getenv("SUN_PATH")) {
      auto cwd = std::filesystem::current_path();
      setenv("SUN_PATH", cwd.c_str(), 0);
    }
    // Initialize library cache from environment
    sun::LibraryCache::instance().initFromEnvironment();
  });
}

// Execute and log SunError to stderr, then rethrow
inline sun::SunValue executeString(const std::string& source, int argc = 0,
                                   char** argv = nullptr) {
  ensureSunPathSet();
  try {
    auto driver = Driver::createForJIT();
    driver->setDumpIR(true);  // Dump IR for debugging
    return driver->executeString(source, argc, argv);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

inline sun::SunValue executeFile(const std::string& filename, int argc = 0,
                                 char** argv = nullptr) {
  try {
    auto driver = Driver::createForJIT();
    driver->executeFile(std::filesystem::absolute(filename).string(), argc,
                        argv);
    return sun::VoidValue{};
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

inline void compileFile(const std::string& filename) {
  try {
    ensureSunPathSet();
    Driver::createForAOT()->compileFile(
        std::filesystem::absolute(filename).string());
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}

inline void compileString(const std::string& source) {
  try {
    Driver::createForAOT("test_compile")->compileString(source);
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    throw;
  }
}