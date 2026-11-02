#include <gtest/gtest.h>
#include <llvm/TargetParser/Host.h>

#include <filesystem>
#include <fstream>

#include "driver/build_graph.h"
#include "driver/manifest_processor.h"
#include "driver/package.h"
#include "support/error.h"

/** Keeps composed-config fixtures independent of installed packages. */
namespace {
/** Shortens filesystem operations in test fixtures. */
namespace fs = std::filesystem;
using sun::driver::SunConfig;
using sun::support::SunError;

/** Owns isolated graph inputs and restores the compiler callback. */
class BuildGraph : public testing::Test {
 protected:
  fs::path dir;
  /** Creates a fresh fixture beneath the project's temporary directory. */
  void SetUp() override {
    dir = fs::current_path() / "tmp/build-graph-tests" /
          testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(dir);
    fs::create_directories(dir);
    sun::driver::ManifestProcessor::clearPathVariables();
  }
  /** Removes callbacks holding references to this fixture. */
  void TearDown() override { sun::driver::setProductCompiler({}); }
  /** Writes a config or source fixture relative to this test's directory. */
  void put(const fs::path& relative, const std::string& contents) {
    auto path = dir / relative;
    fs::create_directories(path.parent_path());
    std::ofstream(path) << contents;
  }
};

/** Including a config preserves its own variables and canonicalizes diamonds.
 */
TEST_F(BuildGraph, CompositionIsExplicitAndDoesNotMergeSettings) {
  put("sun-config.json",
      R"({"projects":["projects/*/sun-config.json"],"path_variables":{"ROOT":"root"}})");
  put("projects/a/sun-config.json",
      R"({"package_id":"example/a","projects":["../../shared/sun-config.json"],"path_variables":{"VALUE":"a"}})");
  put("projects/b/sun-config.json",
      R"({"package_id":"example/b","projects":["../../shared/sun-config.json"]})");
  put("shared/sun-config.json", R"({"package_id":"example/shared"})");
  put("ignored/sun-config.json", "invalid JSON deliberately excluded");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  EXPECT_EQ(graph.projects.size(), 4u);
  auto child = SunConfig::loadFile(dir / "projects/a/sun-config.json");
  EXPECT_EQ(child.pathVariables.count("ROOT"), 0u);
  EXPECT_EQ(child.pathVariables.at("VALUE"), (dir / "projects/a/a").string());
}

/** Artifact filenames resolve beneath the selected root, including child
 * configs. */
TEST_F(BuildGraph, ArtifactNamesUseSelectedBuildRoot) {
  put("sun-config.json", R"({"projects":["child/sun-config.json"]})");
  put("child/sun-config.json",
      R"({"package_id":"example/child","entrypoints":[{"path":"main.sun","name":"tool"}]})");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  auto raw = SunConfig::loadFile(dir / "child/sun-config.json");
  EXPECT_EQ(raw.entrypoints.front().name, "tool");
  auto configured = sun::driver::configureBuildPaths(graph, raw);
  const fs::path output = configured.pathVariables.at("SUN_BUILD");
  EXPECT_EQ(configured.entrypoints.front().outputName,
            (output / "tool").string());
  EXPECT_EQ(configured.entrypoints.front().testBinaryName,
            (output / "tool_test").string());
  const auto expected = dir / ".sun/build" /
                        sun::driver::configTargetKey(raw.targetTriple) /
                        "release/example/child";
  EXPECT_EQ(output, expected);
  EXPECT_EQ(
      configured.pathVariables.at("SUN_INSTALL"),
      (dir / ".sun/install" / sun::driver::configTargetKey(raw.targetTriple) /
       "release/example/child")
          .string());
}

/** Manifest identity selects a product without determining its artifact
 * filename. */
TEST_F(BuildGraph, ManifestIdentityDoesNotNameArtifacts) {
  put("sun-config.json",
      R"({"package_id":"example/package","entrypoints":[{"name":"local-name","path":"api.sun","type":"library","export":true}]})");
  put("api.sun", "manifest { id: \"example/public-api\" }\n");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  sun::driver::setProductCompiler(
      [](const auto&, const auto&, const fs::path& output) {
        EXPECT_EQ(output.filename(), "local-name.moon");
        std::ofstream(output) << "library";
      });
  const auto installed = sun::driver::buildConfigProducts(
      graph, SunConfig::loadFile(dir / "sun-config.json"));
  EXPECT_TRUE(fs::is_regular_file(installed / "lib/local-name.moon"));
  EXPECT_FALSE(fs::exists(installed / "lib/public-api.moon"));
}

/** Inclusion cycles and ambiguous package identities fail before compilation.
 */
TEST_F(BuildGraph, RejectsCyclesAndDuplicateProviders) {
  put("sun-config.json", R"({"projects":["child/sun-config.json"]})");
  put("child/sun-config.json", R"({"projects":["../sun-config.json"]})");
  EXPECT_THROW(sun::driver::loadConfigGraph(dir / "sun-config.json"), SunError);
  put("sun-config.json",
      R"({"package_id":"example/duplicate","projects":["child/sun-config.json"]})");
  put("child/sun-config.json", R"({"package_id":"example/duplicate"})");
  EXPECT_THROW(sun::driver::loadConfigGraph(dir / "sun-config.json"), SunError);
}

/** A local provider takes precedence over a nonexistent external artifact. */
TEST_F(BuildGraph, LocalProductWinsAndMissingSelectionsDoNotFallBack) {
  put("sun-config.json",
      R"({"projects":["producer/sun-config.json","consumer/sun-config.json"]})");
  put("producer/sun-config.json",
      R"({"package_id":"example/tools","entrypoints":[{"name":"generator","type":"binary","path":"tool.sun","export":true}]})");
  put("producer/tool.sun",
      "manifest { id: \"example/generator\" }\nfunction main() i32 { return 0; "
      "}\n");
  put("consumer/sun-config.json",
      R"({"dependencies":[{"package":"example/tools","binary":"example/generator","path":"does-not-exist"}]})");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  size_t builds = 0;
  sun::driver::setProductCompiler(
      [&](const auto&, const auto&, const fs::path& output) {
        ++builds;
        std::ofstream(output) << "test executable";
      });
  auto consumer = SunConfig::loadFile(dir / "consumer/sun-config.json");
  auto path = sun::driver::ManifestProcessor::expandPathVariables(
      "${example/tools}/bin/generator", &consumer);
  EXPECT_TRUE(fs::is_regular_file(path));
  EXPECT_EQ(builds, 1u);
  consumer.dependencies.at("example/tools").selections.front().binary =
      "example/missing";
  EXPECT_THROW(sun::driver::resolveConfigDependency(consumer, "example/tools"),
               SunError);
}

/** Dependencies are a uniform array with mutually exclusive product selectors.
 */
TEST_F(BuildGraph, ValidatesDependencyArray) {
  put("sun-config.json",
      R"({"dependencies":[{"package":"example/ui","library":"example/ui","path":"ui.moon"},{"package":"example/ui","binary":"example/viewc","path":"viewc"}]})");
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  ASSERT_EQ(config.dependencies.at("example/ui").selections.size(), 2u);
  put("sun-config.json",
      R"({"dependencies":{"UI":{"moon":{"path":"ui.moon"}}}})");
  EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
  put("sun-config.json",
      R"({"dependencies":[{"package":"example/ui","library":"example/ui","binary":"example/ui","path":"ui"}]})");
  EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
}

/** Product identity comes from the same manifest field for both product kinds.
 */
TEST_F(BuildGraph, ReadsUnifiedManifestIdentity) {
  put("tool.sun",
      "manifest { id: \"example/tool\" }\nfunction main() i32 { return 0; }\n");
  sun::driver::ConfigEntrypoint entry;
  entry.path = (dir / "tool.sun").string();
  EXPECT_EQ(sun::driver::entrypointIdentity(entry), "example/tool");
}
/** Tracked preparation reruns when input bytes change and preserves good
 * installs on failure. */
TEST_F(BuildGraph, TracksPreparationAndPreservesPublishedProducts) {
  put("sun-config.json", R"({"package_id":"example/generated",
    "steps":[{"name":"generate","command":["/bin/cp","input.txt","${SUN_BUILD}/generated.txt"],
      "inputs":["input.txt"],"outputs":["${SUN_BUILD}/generated.txt"]}],
    "entrypoints":[{"name":"generated","path":"main.sun","type":"library","export":true,"steps":["generate"]}]})");
  put("main.sun", "manifest { id: \"example/generated\" }\n");
  put("input.txt", "first");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  sun::driver::setProductCompiler(
      [](const auto& config, const auto&, const fs::path& output) {
        fs::copy_file(
            fs::path(config.pathVariables.at("SUN_BUILD")) / "generated.txt",
            output, fs::copy_options::overwrite_existing);
      });
  auto installed = sun::driver::buildConfigProducts(graph, config);
  const auto first =
      sun::driver::packageFileHash(installed / "lib/generated.moon");
  auto selected = sun::driver::configureBuildPaths(graph, config);
  const auto generated =
      fs::path(selected.pathVariables.at("SUN_BUILD")) / "generated.txt";
  const auto time = fs::last_write_time(generated);
  sun::driver::buildConfigProducts(graph, config);
  EXPECT_EQ(fs::last_write_time(generated), time);
  put("input.txt", "second");
  sun::driver::buildConfigProducts(graph, config);
  const auto second =
      sun::driver::packageFileHash(installed / "lib/generated.moon");
  EXPECT_NE(first, second);
  fs::remove(dir / "input.txt");
  EXPECT_THROW(sun::driver::buildConfigProducts(graph, config), SunError);
  EXPECT_EQ(sun::driver::packageFileHash(installed / "lib/generated.moon"),
            second);
}

/** Build-step tool references select host products even for a foreign target.
 */
TEST_F(BuildGraph, SelectsHostGeneratorFromUsage) {
  put("sun-config.json",
      R"({"projects":["tool/sun-config.json","app/sun-config.json"]})");
  put("tool/sun-config.json",
      R"({"package_id":"example/tools","entrypoints":[{"name":"tool","path":"main.sun","type":"binary","export":true}]})");
  put("tool/main.sun", "manifest { id: \"example/tool\" }\n");
  put("app/sun-config.json", R"({"package_id":"example/app",
    "dependencies":[{"package":"example/tools","binary":"example/tool","path":"missing"}],
    "steps":[{"name":"generate","command":["${example/tools}/bin/tool","input.txt","${SUN_BUILD}/generated.txt"],
      "inputs":["input.txt"],"outputs":["${SUN_BUILD}/generated.txt"]}],
    "entrypoints":[{"name":"app","path":"main.sun","type":"library","export":true,"steps":["generate"]}]})");
  put("app/main.sun", "manifest { id: \"example/app\" }\n");
  put("app/input.txt", "generated");
  const auto target =
      sun::driver::configTargetKey(llvm::sys::getDefaultTargetTriple()) ==
              sun::driver::configTargetKey("x86_64-linux-gnu")
          ? "aarch64-linux-gnu"
          : "x86_64-linux-gnu";
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json", target);
  sun::driver::ConfigGraphScope scope(graph);
  bool builtHost = false;
  sun::driver::setProductCompiler([&](const auto& config, const auto&,
                                      const fs::path& output) {
    if (config.packageId == "example/tools") {
      EXPECT_EQ(
          sun::driver::configTargetKey(config.targetTriple),
          sun::driver::configTargetKey(llvm::sys::getDefaultTargetTriple()));
      std::ofstream(output) << "#!/bin/sh\ncp \"$1\" \"$2\"\n";
      builtHost = true;
    } else {
      EXPECT_EQ(sun::driver::configTargetKey(config.targetTriple),
                sun::driver::configTargetKey(target));
      std::ofstream(output) << "target library";
    }
  });
  auto config = SunConfig::loadFile(dir / "app/sun-config.json", target);
  sun::driver::buildConfigProducts(graph, config);
  EXPECT_TRUE(builtHost);
}

}  // namespace
