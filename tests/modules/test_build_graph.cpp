#include <gtest/gtest.h>
#include <llvm/TargetParser/Host.h>

#include <filesystem>
#include <fstream>

#include "cli/config_build_command.h"
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

/** Package identities form relative paths with nonempty portable components. */
TEST_F(BuildGraph, PackageIdentityAcceptsPortablePaths) {
  for (const auto* id : {"sun", "namo-robotics/sun", "org/tools/sun_v1.2"}) {
    SCOPED_TRACE(id);
    put("sun-config.json", std::string("{\"package_id\":\"") + id + "\"}");
    EXPECT_EQ(SunConfig::loadFile(dir / "sun-config.json").packageId, id);
  }
}

/** Unsafe paths and whitespace cannot become package output directories. */
TEST_F(BuildGraph, PackageIdentityRejectsUnsafePaths) {
  for (const auto* value : {
           R"("")", R"("org/my package")", R"(" org/sun")", R"("org/sun ")",
           R"("org/\tsun")", R"("org/\nsun")", R"("org/\u0000sun")",
           R"("/org/sun")", R"("org//sun")", R"("org/sun/")",
           R"(".")", R"("..")", R"("../sun")", R"("org/../sun")",
           R"("org/./sun")", R"("org\\sun")", R"("C:/sun")",
           R"("org/$SUN_BUILD")", R"("org/sun*")", R"("org/sun?")"}) {
    SCOPED_TRACE(value);
    put("sun-config.json", std::string("{\"package_id\":") + value + "}");
    EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
  }
}

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

/** Entrypoint names distinguish products even when source and manifest ID match. */
TEST_F(BuildGraph, SharedIdentityBuildsDistinctEntrypoints) {
  put("sun-config.json", R"({"entrypoints":[
    {"name":"second","path":"api.sun","type":"library","requires":["first"]},
    {"name":"first","path":"api.sun","type":"library"}]})");
  put("api.sun", "manifest { id: \"example/api\" }\n");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  std::vector<std::string> built;
  sun::driver::setProductCompiler(
      [&](const auto&, const auto& entry, const fs::path& output) {
        built.push_back(entry.name);
        std::ofstream(output) << entry.name;
      });
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  auto installed = sun::driver::buildConfigProducts(graph, config, {}, "first");
  EXPECT_EQ(built, (std::vector<std::string>{"first"}));
  EXPECT_FALSE(fs::exists(installed / "lib/second.moon"));
  built.clear();
  sun::driver::buildConfigProducts(graph, config);
  EXPECT_EQ(built, (std::vector<std::string>{"first", "second"}));
  EXPECT_TRUE(fs::is_regular_file(installed / "lib/first.moon"));
  EXPECT_TRUE(fs::is_regular_file(installed / "lib/second.moon"));
}

/** ID selectors reject ambiguity among products of the requested kind. */
TEST_F(BuildGraph, SharedIdentityRequiresUnambiguousSelection) {
  put("sun-config.json", R"({"entrypoints":[
    {"name":"first","path":"api.sun","type":"library"},
    {"name":"second","path":"api.sun","type":"library"},
    {"name":"tool","path":"api.sun","type":"binary"}]})");
  put("api.sun", "manifest { id: \"example/api\" }\n");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  std::vector<std::string> built;
  sun::driver::setProductCompiler(
      [&](const auto&, const auto& entry, const fs::path& output) {
        built.push_back(entry.name);
        std::ofstream(output) << entry.name;
      });
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  sun::driver::ConfigDependency selection;
  selection.package = false;
  selection.library = "example/api";
  try {
    sun::driver::buildConfigProducts(graph, config, {selection});
    FAIL() << "expected an ambiguous product diagnostic";
  } catch (const SunError& error) {
    EXPECT_NE(std::string(error.what()).find("ambiguous product id: example/api"),
              std::string::npos);
  }
  EXPECT_TRUE(built.empty());
  selection.library.clear();
  selection.binary = "example/api";
  sun::driver::buildConfigProducts(graph, config, {selection});
  EXPECT_EQ(built, (std::vector<std::string>{"tool"}));
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
  EXPECT_FALSE(sun::driver::isCurrentlyBuildingDependency());
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

/** Direct builds copy resources and refresh only changed files. */
TEST_F(BuildGraph, CopiesRuntimeResourcesToBuildAndInstall) {
  put("sun-config.json", R"({"package_id":"example/app",
    "entrypoints":[{"name":"app","path":"main.sun","export":true,
    "resources":[{"path":"icons","destination":"app.icons"}]}]})");
  put("icons/home.svg", "first");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  sun::driver::setProductCompiler(
      [&](const auto&, const auto&, const fs::path& output) {
        std::ofstream(output) << "binary";
      });
  auto installed = sun::driver::buildConfigProducts(graph, config);
  auto selected = sun::driver::configureBuildPaths(graph, config);
  const fs::path build = selected.pathVariables.at("SUN_BUILD");
  const auto icon = build / "app.icons/home.svg";
  ASSERT_TRUE(fs::is_regular_file(icon));
  EXPECT_EQ(sun::driver::packageFileHash(icon),
            sun::driver::packageFileHash(installed / "bin/app.icons/home.svg"));
  const auto time = fs::last_write_time(icon);
  sun::driver::buildConfigProducts(graph, config);
  EXPECT_EQ(fs::last_write_time(icon), time);
  put("icons/home.svg", "changed");
  sun::driver::buildConfigProducts(graph, config);
  EXPECT_EQ(sun::driver::packageFileHash(icon),
            sun::driver::packageFileHash(dir / "icons/home.svg"));
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

/** Local dependency builds skip test sources while direct builds still use them. */
TEST_F(BuildGraph, SkipsLocalDependencyTestsOnlyDuringResolution) {
  put("sun-config.json", R"({
    "projects": [
      "producer/sun-config.json",
      "consumer/sun-config.json"
    ]
  })");

  put("producer/sun-config.json", R"({
    "package_id": "example/library",
    "entrypoints": [
      {
        "name": "api",
        "path": "api.sun",
        "type": "library",
        "export": true,
        "compile_args": ["-O0"]
      }
    ]
  })");

  // The missing test source makes it observable whether tests are processed.
  put("producer/api.sun", R"(
    manifest {
      id: "example/api",
      test_files: ["missing_test.sun"]
    }

    /** Supplies a dependency fixture. */
    public module api {}
  )");

  put("consumer/sun-config.json", R"({
    "dependencies": [
      {
        "package": "example/library",
        "library": "example/api",
        "path": "missing.moon"
      }
    ]
  })");

  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  sun::cli::BuildRunOptions options;
  options.forceRebuild = true;
  sun::cli::configureProductCompiler(options);

  // Resolving the local library must succeed without reading its test source.
  auto consumer = SunConfig::loadFile(dir / "consumer/sun-config.json");
  const auto installed =
      sun::driver::resolveConfigDependency(consumer, "example/library");

  EXPECT_TRUE(fs::is_regular_file(installed / "lib/api.moon"));
  EXPECT_FALSE(sun::driver::isCurrentlyBuildingDependency());

  // Building the producer directly must still attempt to compile its tests.
  auto producer = SunConfig::loadFile(dir / "producer/sun-config.json");
  EXPECT_THROW(sun::driver::buildConfigProducts(graph, producer, {}, "api"),
               SunError);
}

/** Custom local dependency builds receive the same test policy as compilation. */
TEST_F(BuildGraph, SkipsTestsInLocalDependencyCustomBuilds) {
  put("sun-config.json", R"({
    "projects": [
      "producer/sun-config.json",
      "consumer/sun-config.json"
    ]
  })");

  put("producer/sun-config.json", R"({
    "package_id": "example/tools",
    "entrypoints": [
      {
        "name": "tool",
        "path": "tool.sun",
        "type": "binary",
        "export": true,
        "build": ["build.py"]
      }
    ]
  })");

  put("producer/tool.sun", R"(
    manifest {
      id: "example/tool"
    }
  )");

  put("producer/build.py", R"PY(#!/usr/bin/python3
"""Require dependency test suppression before producing the tool."""
import json
import os
import pathlib

context_path = pathlib.Path(os.environ["SUN_BUILD_CONTEXT"])
context = json.loads(context_path.read_text())
assert "--no-test" in context["flags"]

output_path = pathlib.Path(os.environ["SUN_OUTPUT"])
output_path.write_text("#!/bin/sh\nexit 0\n")
)PY");
  fs::permissions(dir / "producer/build.py", fs::perms::owner_all);

  put("consumer/sun-config.json", R"({
    "dependencies": [
      {
        "package": "example/tools",
        "binary": "example/tool",
        "path": "missing"
      }
    ]
  })");

  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  sun::cli::configureProductCompiler({});

  // The script fails unless dependency resolution passes --no-test.
  auto consumer = SunConfig::loadFile(dir / "consumer/sun-config.json");
  const auto installed =
      sun::driver::resolveConfigDependency(consumer, "example/tools");

  EXPECT_TRUE(fs::is_regular_file(installed / "bin/tool"));
}

}  // namespace
