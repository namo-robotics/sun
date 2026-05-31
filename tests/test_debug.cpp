// tests/test_debug.cpp - Tests for debug/visualization features

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "driver.h"
#include "error.h"
#include "execution_utils.h"

// ============================================================================
// Debug Mode Tests
// ============================================================================

TEST(DebugTest, debug_mode_generates_scope_html) {
  initTestEnvironment();

  // Compile a simple program with debug mode enabled
  auto driver = Driver::createForJIT();
  driver->setDebugMode(true, "test_debug");

  std::string source = R"(
    function main() i32 {
      return 42;
    }
  )";

  // Execute - this should generate the debug HTML file
  auto result = driver->executeString(source);
  EXPECT_EQ(std::get<int>(result), 42);

  // Check that the debug folder and scope tree HTML was created
  std::string debugFolder = "test_debug_debug";
  std::string scopeFile = debugFolder + "/scope_tree.html";
  EXPECT_TRUE(std::filesystem::exists(scopeFile))
      << "Debug file " << scopeFile << " should be created";

  // Clean up
  if (std::filesystem::exists(debugFolder)) {
    std::filesystem::remove_all(debugFolder);
  }
}
