// tests/tooling/backend/test_debug.cpp - Tests for debug/visualization features

#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>
#include <llvm/Support/JSON.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "support/error.h"

// ============================================================================
// Debug Mode Tests
// ============================================================================

TEST(Tooling_Backend_Debug, debug_mode_generates_scope_html) {
  sun::driver::initTestEnvironment();

  // Compile a simple program with debug mode enabled. Unique name per
  // process so parallel test runs don't share the debug output folder.
  std::string debugName = "test_debug_" + std::to_string(getpid());
  auto driver = sun::driver::Driver::createForJIT();
  driver->setDebugMode(true, debugName);

  std::string source = R"(
    function main() i32 {
      return 42;
    }
  )";

  // Execute - this should generate the debug HTML file
  auto result = driver->executeString(source);
  EXPECT_EQ(std::get<int>(result), 42);

  // Check that the debug folder and scope tree HTML was created
  std::string debugFolder = debugName + "_debug";
  std::string scopeFile = debugFolder + "/scope_tree.html";
  EXPECT_TRUE(std::filesystem::exists(scopeFile))
      << "Debug file " << scopeFile << " should be created";

  // Clean up
  if (std::filesystem::exists(debugFolder)) {
    std::filesystem::remove_all(debugFolder);
  }
}

/** Checks that debug builds inspect the written bundle even after a cache hit.
 */
TEST(Tooling_Backend_Debug, moon_json_describes_written_metadata) {
  sun::driver::initTestEnvironment();
  namespace fs = std::filesystem;
  using namespace sun::moon_bundling;
  const auto dir = fs::path("tmp") / ("moon_debug_" + std::to_string(getpid()));
  fs::create_directories(dir);
  const auto source = dir / "library.sun";
  const auto bundle = dir / "custom.moon";
  std::ofstream(source) << R"(
    /** Supplies metadata for the debug JSON test. */
    public module inspection {
      /** Returns the test value. */
      public function answer() i32 { return 42; }
    }
  )";
  MoonBuildOptions options;
  MoonBuilder::build(source.string(), bundle, options);
  EXPECT_FALSE(fs::exists(dir / "library_debug" / "moon.json"));
  EXPECT_TRUE(MoonBuilder::build(source.string(), bundle, options).upToDate);
  options.debugMode = true;
  EXPECT_FALSE(MoonBuilder::build(source.string(), bundle, options).upToDate);
  const auto jsonPath = dir / "library_debug" / "moon.json";
  std::ifstream input(jsonPath);
  ASSERT_TRUE(input);
  const std::string json((std::istreambuf_iterator<char>(input)), {});
  auto document = llvm::json::parse(json);
  ASSERT_TRUE(static_cast<bool>(document))
      << llvm::toString(document.takeError());
  const auto* root = document->getAsObject();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getInteger("format_version"), MoonHeader::VERSION);
  ASSERT_NE(root->getArray("native_archives"), nullptr);
  EXPECT_TRUE(root->getArray("native_archives")->empty());
  const auto* modules = root->getArray("modules");
  ASSERT_NE(modules, nullptr);
  ASSERT_FALSE(modules->empty());
  auto reader = MoonReader::open(bundle);
  ASSERT_NE(reader, nullptr);
  bool found = false;
  for (const auto& value : *modules) {
    const auto* entry = value.getAsObject();
    ASSERT_NE(entry, nullptr);
    const auto* metadata = entry->getObject("metadata");
    ASSERT_NE(metadata, nullptr);
    if (metadata->getString("module_name") != "inspection") continue;
    found = true;
    EXPECT_GT(entry->getInteger("bitcode_size").value_or(0), 0);
    ASSERT_TRUE(entry->getString("module_key"));
    const auto* declarations = metadata->getArray("declarations");
    ASSERT_NE(declarations, nullptr);
    for (const auto& declaration : *declarations) {
      const auto* record = declaration.getAsObject();
      ASSERT_NE(record, nullptr);
      ASSERT_TRUE(record->getString("kind"));
      if (record->getString("name") == "inspection")
        EXPECT_EQ(record->getString("kind"), "MODULE");
      if (record->getString("name") == "answer")
        EXPECT_EQ(record->getString("kind"), "FUNCTION");
      ASSERT_TRUE(record->getString("key"));
      EXPECT_TRUE(record->getString("key")->starts_with("$"));
    }
    const auto* original =
        reader->getMetadata(entry->getString("module_key")->str());
    ASSERT_NE(original, nullptr);
    sun::moon::ModuleMetadata restored;
    std::string metadataJson;
    llvm::raw_string_ostream stream(metadataJson);
    stream << *entry->get("metadata");
    ASSERT_TRUE(
        google::protobuf::util::JsonStringToMessage(metadataJson, &restored)
            .ok());
    EXPECT_EQ(restored.SerializeAsString(), original->SerializeAsString());
    ASSERT_EQ(restored.functions_size(), 1);
    EXPECT_EQ(restored.functions(0).proto().name(), "answer");
  }
  EXPECT_TRUE(found);
  EXPECT_TRUE(fs::exists(dir / "library_debug" / "ir.ll"));
  EXPECT_FALSE(reader->writeDebugJson(dir / "missing" / "moon.json"));
  EXPECT_NE(reader->getError().find("Failed to write"), std::string::npos);
  fs::remove_all(dir);
}
