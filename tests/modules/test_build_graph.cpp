#include <gtest/gtest.h>
#include <llvm/Support/FormatVariadic.h>
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
  /** Reads this test's JSON fixture from the source tree. */
  std::string readConfigFixture(const std::string& filename) {
    const auto path =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "fixtures/build_graph" /
        testing::UnitTest::GetInstance()->current_test_info()->name() /
        filename;
    std::ifstream input(path);
    if (!input) {
      sun::support::logAndThrowError("cannot read test config " +
                                     path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), {});
  }

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
  auto fixture = llvm::json::parse(readConfigFixture("cases.json"));
  ASSERT_TRUE(static_cast<bool>(fixture))
      << llvm::toString(fixture.takeError());
  ASSERT_NE(fixture->getAsObject(), nullptr);
  for (const auto& [name, config] : *fixture->getAsObject()) {
    SCOPED_TRACE(name.str());
    put("sun-config.json", llvm::formatv("{0:2}", config).str());
    EXPECT_EQ(SunConfig::loadFile(dir / "sun-config.json").packageId,
              config.getAsObject()->getString("package_id")->str());
  }
}

/** Unsafe paths and whitespace cannot become package output directories. */
TEST_F(BuildGraph, PackageIdentityRejectsUnsafePaths) {
  auto fixture = llvm::json::parse(readConfigFixture("cases.json"));
  ASSERT_TRUE(static_cast<bool>(fixture))
      << llvm::toString(fixture.takeError());
  ASSERT_NE(fixture->getAsObject(), nullptr);
  for (const auto& [name, config] : *fixture->getAsObject()) {
    SCOPED_TRACE(name.str());
    put("sun-config.json", llvm::formatv("{0:2}", config).str());
    EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
  }
}

/** Including a config preserves its own variables and canonicalizes diamonds.
 */
TEST_F(BuildGraph, CompositionIsExplicitAndDoesNotMergeSettings) {
  put("sun-config.json", readConfigFixture("workspace.json"));
  put("projects/a/sun-config.json", readConfigFixture("a.json"));
  put("projects/b/sun-config.json", readConfigFixture("b.json"));
  put("shared/sun-config.json", readConfigFixture("shared.json"));
  put("ignored/sun-config.json", readConfigFixture("invalid.json"));
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  EXPECT_EQ(graph.projects.size(), 4u);
  auto child = SunConfig::loadFile(dir / "projects/a/sun-config.json");
  EXPECT_EQ(child.pathVariables.count("ROOT"), 0u);
  EXPECT_EQ(child.pathVariables.at("VALUE"), (dir / "projects/a/a").string());
}

/** Unsupported projects warn once and never resolve their paths or children. */
TEST_F(BuildGraph, SkipsUnsupportedProjects) {
  put("sun-config.json", R"({"projects":["linux.json","linux.json","portable.json"]})");
  put("linux.json", R"({
    "supported_targets":["x86_64-linux-gnu","x86_64-linux-musl"],
    "projects":["missing.json"],
    "path_variables":{"LIB":{"target":{"x86_64-linux-gnu":"/missing"}}}
  })");
  put("portable.json", R"({"package_id":"example/portable"})");
  testing::internal::CaptureStderr();
  const auto graph = sun::driver::loadConfigGraph(
      dir / "sun-config.json", "arm64-apple-darwin");
  const auto warning = testing::internal::GetCapturedStderr();
  ASSERT_EQ(graph.projects.size(), 2u);
  EXPECT_EQ(graph.projects.back(), fs::canonical(dir / "portable.json"));
  ASSERT_EQ(graph.skippedProjects.size(), 1u);
  EXPECT_NE(warning.find("Skipping project"), std::string::npos);
  EXPECT_NE(warning.find("linux.json"), std::string::npos);
  EXPECT_NE(warning.find("aarch64-unknown-darwin"), std::string::npos);
}

/** Supported projects remain available for an explicitly selected target. */
TEST_F(BuildGraph, IncludesSupportedProjects) {
  put("sun-config.json", R"({"projects":["linux.json"]})");
  put("linux.json", R"({"supported_targets":["x86_64-linux-musl"]})");
  const auto graph = sun::driver::loadConfigGraph(
      dir / "sun-config.json", "x86_64-unknown-linux-musl");
  EXPECT_EQ(graph.projects.size(), 2u);
  EXPECT_TRUE(graph.skippedProjects.empty());
}

/** A build with only unsupported projects completes after its warning. */
TEST_F(BuildGraph, UnsupportedRootBuildSucceedsWithoutProducts) {
  put("sun-config.json", R"({"supported_targets":["x86_64-linux-musl"]})");
  EXPECT_EQ(sun::cli::runBuildCommand(
                {"--config", (dir / "sun-config.json").string(),
                 "--target", "arm64-apple-darwin"}), 0);
}

/** Artifact filenames resolve beneath the selected root, including child
 * configs. */
TEST_F(BuildGraph, ArtifactNamesUseSelectedBuildRoot) {
  put("sun-config.json", readConfigFixture("../shared/child-workspace.json"));
  put("child/sun-config.json", readConfigFixture("child.json"));
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

/** Entrypoint names determine artifact filenames independently of manifest IDs.
 */
TEST_F(BuildGraph, ManifestIdentityDoesNotNameArtifacts) {
  put("sun-config.json", readConfigFixture("workspace.json"));
  put("api.sun", "manifest { id: \"example/public-api\" }\n");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  sun::driver::setProductCompiler(
      [](const auto&, const auto&, const fs::path& output) {
        EXPECT_EQ(output.filename(), "local-name.moon");
        std::ofstream(output) << "library";
      });
  const auto installed = sun::driver::buildConfigProducts(
      graph, SunConfig::loadFile(dir / "sun-config.json"), {"local-name"});
  EXPECT_TRUE(fs::is_regular_file(installed / "lib/local-name.moon"));
  EXPECT_FALSE(fs::exists(installed / "lib/public-api.moon"));
}

/** Entrypoint names distinguish products even when source and manifest ID
 * match. */
TEST_F(BuildGraph, SharedIdentityBuildsDistinctEntrypoints) {
  put("sun-config.json", readConfigFixture("workspace.json"));
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
  auto installed = sun::driver::buildConfigProducts(graph, config, {"first"});
  EXPECT_EQ(built, (std::vector<std::string>{"first"}));
  EXPECT_FALSE(fs::exists(installed / "lib/second.moon"));
  built.clear();
  sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  EXPECT_EQ(built, (std::vector<std::string>{"first", "second"}));
  EXPECT_TRUE(fs::is_regular_file(installed / "lib/first.moon"));
  EXPECT_TRUE(fs::is_regular_file(installed / "lib/second.moon"));
}

/** Unknown entrypoint names fail before any product is compiled. */
TEST_F(BuildGraph, RejectsUnknownEntrypointNames) {
  put("sun-config.json", readConfigFixture("workspace.json"));
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  sun::driver::setProductCompiler([](const auto&, const auto&, const auto&) {
    ADD_FAILURE() << "An invalid selection must not invoke the compiler";
  });
  EXPECT_THROW(sun::driver::buildConfigProducts(graph, config, {"missing"}),
               SunError);
}

/** Inclusion cycles and ambiguous package identities fail before compilation.
 */
TEST_F(BuildGraph, RejectsCyclesAndDuplicateProviders) {
  put("sun-config.json", readConfigFixture("../shared/child-workspace.json"));
  put("child/sun-config.json", readConfigFixture("child.json"));
  EXPECT_THROW(sun::driver::loadConfigGraph(dir / "sun-config.json"), SunError);
  put("sun-config.json", readConfigFixture("workspace-2.json"));
  put("child/sun-config.json", readConfigFixture("child-2.json"));
  EXPECT_THROW(sun::driver::loadConfigGraph(dir / "sun-config.json"), SunError);
}

/** A local provider takes precedence over a nonexistent external artifact. */
TEST_F(BuildGraph, LocalProductWinsAndMissingSelectionsDoNotFallBack) {
  put("sun-config.json",
      readConfigFixture("../shared/producer-consumer-workspace.json"));
  put("producer/sun-config.json", readConfigFixture("producer.json"));
  put("producer/tool.sun",
      "manifest { id: \"example/generator\" }\nfunction main() i32 { return 0; "
      "}\n");
  put("consumer/sun-config.json", readConfigFixture("consumer.json"));
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
  consumer.dependencies.at("example/tools").entrypoints = {"missing"};
  EXPECT_THROW(sun::driver::resolveConfigDependency(consumer, "example/tools"),
               SunError);
  EXPECT_FALSE(sun::driver::isCurrentlyBuildingDependency());
}

/** Dependencies are a uniform array with mutually exclusive product selectors.
 */
TEST_F(BuildGraph, ValidatesDependencyArray) {
  put("sun-config.json", readConfigFixture("consumer.json"));
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  ASSERT_EQ(config.dependencies.at("example/ui").selections.size(), 2u);
  put("sun-config.json", readConfigFixture("consumer-2.json"));
  EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
  put("sun-config.json", readConfigFixture("consumer-3.json"));
  EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
}

/** Tracked preparation reruns when input bytes change and preserves good
 * installs on failure. */
TEST_F(BuildGraph, TracksPreparationAndPreservesPublishedProducts) {
  put("sun-config.json", readConfigFixture("workspace.json"));
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
  auto installed = sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  const auto first =
      sun::driver::packageFileHash(installed / "lib/generated.moon");
  auto selected = sun::driver::configureBuildPaths(graph, config);
  const auto generated =
      fs::path(selected.pathVariables.at("SUN_BUILD")) / "generated.txt";
  const auto time = fs::last_write_time(generated);
  sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  EXPECT_EQ(fs::last_write_time(generated), time);
  put("input.txt", "second");
  sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  const auto second =
      sun::driver::packageFileHash(installed / "lib/generated.moon");
  EXPECT_NE(first, second);
  fs::remove(dir / "input.txt");
  EXPECT_THROW(
      sun::driver::buildConfigProducts(
          graph, config, sun::driver::defaultConfigEntrypoints(config)),
      SunError);
  EXPECT_EQ(sun::driver::packageFileHash(installed / "lib/generated.moon"),
            second);
}

/** Direct builds copy resources and refresh only changed files. */
TEST_F(BuildGraph, CopiesRuntimeResourcesToBuildAndInstall) {
  put("sun-config.json", readConfigFixture("workspace.json"));
  put("icons/home.svg", "first");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  sun::driver::setProductCompiler(
      [&](const auto&, const auto&, const fs::path& output) {
        std::ofstream(output) << "binary";
      });
  auto installed = sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  auto selected = sun::driver::configureBuildPaths(graph, config);
  const fs::path build = selected.pathVariables.at("SUN_BUILD");
  const auto icon = build / "app.icons/home.svg";
  ASSERT_TRUE(fs::is_regular_file(icon));
  EXPECT_EQ(sun::driver::packageFileHash(icon),
            sun::driver::packageFileHash(installed / "bin/app.icons/home.svg"));
  const auto time = fs::last_write_time(icon);
  sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  EXPECT_EQ(fs::last_write_time(icon), time);
  put("icons/home.svg", "changed");
  sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  EXPECT_EQ(sun::driver::packageFileHash(icon),
            sun::driver::packageFileHash(dir / "icons/home.svg"));
}

/** Build-step tool references select host products even for a foreign target.
 */
TEST_F(BuildGraph, SelectsHostGeneratorFromUsage) {
  put("sun-config.json", readConfigFixture("workspace.json"));
  put("tool/sun-config.json", readConfigFixture("tool.json"));
  put("tool/main.sun", "manifest { id: \"example/tool\" }\n");
  put("app/sun-config.json", readConfigFixture("app.json"));
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
  sun::driver::buildConfigProducts(
      graph, config, sun::driver::defaultConfigEntrypoints(config));
  EXPECT_TRUE(builtHost);
}

/** Local dependency builds skip test sources while direct builds still use
 * them. */
TEST_F(BuildGraph, SkipsLocalDependencyTestsOnlyDuringResolution) {
  put("sun-config.json",
      readConfigFixture("../shared/producer-consumer-workspace.json"));

  put("producer/sun-config.json", readConfigFixture("producer.json"));

  // The missing test source makes it observable whether tests are processed.
  put("producer/api.sun", R"(
    manifest {
      id: "example/api",
      test_files: ["missing_test.sun"]
    }

    /** Supplies a dependency fixture. */
    public module api {}
  )");

  put("consumer/sun-config.json", readConfigFixture("consumer.json"));

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
  EXPECT_THROW(sun::driver::buildConfigProducts(graph, producer, {"api"}),
               SunError);
}

/** Custom local dependency builds receive the same test policy as compilation.
 */
TEST_F(BuildGraph, SkipsTestsInLocalDependencyCustomBuilds) {
  put("sun-config.json",
      readConfigFixture("../shared/producer-consumer-workspace.json"));

  put("producer/sun-config.json", readConfigFixture("producer.json"));

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

  put("consumer/sun-config.json", readConfigFixture("consumer.json"));

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
