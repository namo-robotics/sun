#include <gtest/gtest.h>
#include <llvm/TargetParser/Host.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>

#include "cli/config_build_command.h"
#include "driver/build_graph.h"
#include "driver/build_record.h"
#include "driver/driver.h"
#include "driver/git_source.h"
#include "driver/manifest_processor.h"
#include "driver/package.h"
#include "driver/sun_config.h"
#include "moon_bundling/moon_builder.h"
#include "moon_bundling/moon_cache.h"
#include "support/error.h"
#include "support/sun_path.h"

/** Keeps Git source fixtures private to this test file. */
namespace {
/** Exercises Git orchestration with a fake executable, without network access.
 */
class GitSourceTest : public testing::Test {
 protected:
  std::filesystem::path dir;
  std::filesystem::path originalDirectory;
  std::map<std::string, std::optional<std::string>> environment;
  std::vector<std::filesystem::path> searchPaths;

  /** Saves an environment variable before replacing it for a test. */
  void setEnvironment(const std::string& name, const std::string& value) {
    if (!environment.count(name)) {
      const char* old = std::getenv(name.c_str());
      environment[name] = old ? std::optional<std::string>(old) : std::nullopt;
    }
    setenv(name.c_str(), value.c_str(), 1);
  }

  /** Writes fixture data below the test directory. */
  void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  /** Installs a fake Git that records arguments and materializes fixture
   * source. */
  void SetUp() override {
    originalDirectory = std::filesystem::current_path();
    sun::cli::BuildRunOptions options;
    options.noTest = true;
    sun::cli::configureProductCompiler(options);
    searchPaths = sun::support::SunPath::extraPaths();
    dir = std::filesystem::current_path() / "tmp/git-source-tests" /
          testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write(dir / "bin/git", R"PY(#!/usr/bin/python3
"""Record Git invocations and emulate checkout for compiler integration tests."""
import json
import os
import pathlib
import sys
import shutil
args = sys.argv[1:]
root = pathlib.Path(os.environ['SUN_FAKE_GIT_ROOT'])
with (root / 'calls').open('a') as log:
    log.write(json.dumps(args) + '\n')
if (root / 'fail').exists():
    sys.exit(1)
while args[:1] == ['-c']:
    args = args[2:]
if args[0] == 'init':
    assert args[1:3] == ['--quiet', '--template=']
elif args[2] == 'fetch':
    assert args[3:7] == ['--quiet', '--no-tags', '--depth=1', '--']
    assert len(args) == 9
    (pathlib.Path(args[1]) / 'fixture-repository').write_text(args[-2].split('/')[-1].split(':')[-1])
elif args[2] == 'checkout':
    assert args[3:] == ['--quiet', '--detach', 'FETCH_HEAD']
else:
    raise AssertionError(args)
if 'checkout' in args:
    checkout = pathlib.Path(args[args.index('-C') + 1])
    repository = root / 'repos' / (checkout / 'fixture-repository').read_text()
    if repository.exists():
        shutil.copytree(repository, checkout, dirs_exist_ok=True)
        sys.exit(0)
    (checkout / 'src').mkdir()
    if (root / 'repo-config').exists():
        (checkout / 'sun-config.json').write_text((root / 'repo-config').read_text())
    manifest = (root / 'manifest').read_text() if (root / 'manifest').exists() else ''
    answer = (root / 'answer').read_text() if (root / 'answer').exists() else '42'
    (checkout / 'src/lib.sun').write_text(manifest + '/** Fixture library. */\npublic module fixture { /** Returns a constant. */ public function answer() i32 { return ' + answer + '; } }\n')
)PY");
    std::filesystem::permissions(dir / "bin/git",
                                 std::filesystem::perms::owner_all);
    setEnvironment("SUN_FAKE_GIT_ROOT", dir.string());
    setEnvironment("SUN_GIT_CACHE", (dir / "cache").string());
    const char* path = std::getenv("PATH");
    setEnvironment("PATH", (dir / "bin").string() +
                               (path ? ":" + std::string(path) : ""));
  }

  /** Restores process settings after each test. */
  void TearDown() override {
    std::filesystem::current_path(originalDirectory);
    sun::driver::setProductCompiler({});
    sun::support::SunPath::extraPaths() = searchPaths;
    sun::driver::ManifestProcessor::clearPathVariables();
    for (const auto& [name, value] : environment) {
      if (value) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  /** Creates a library entrypoint with an SSH source and a named version. */
  sun::driver::ConfigEntrypoint entry() {
    sun::driver::ConfigEntrypoint result;
    result.git = "git@example.com:team/library.git";
    result.version = "release/stable";
    result.path = "src/lib.sun";
    result.type = sun::driver::ConfigEntrypoint::Type::Library;
    return result;
  }
};

/** Default Git storage follows the invocation directory, independent of HOME. */
TEST_F(GitSourceTest, DefaultsToWorkingDirectoryCache) {
  unsetenv("SUN_GIT_CACHE");
  setEnvironment("HOME", (dir / "home").string());
  std::filesystem::current_path(dir);
  const auto source = sun::driver::resolveGitEntrypoint(entry());
  const auto relative = std::filesystem::path(source).lexically_relative(
      dir / ".sun/cache/git");
  EXPECT_FALSE(relative.empty());
  EXPECT_NE(*relative.begin(), "..");
  EXPECT_TRUE(std::filesystem::is_regular_file(source));
  EXPECT_FALSE(std::filesystem::exists(dir / "home"));
}

/** Default Moon storage follows the invocation directory, independent of HOME. */
TEST_F(GitSourceTest, DefaultsMoonCacheToWorkingDirectory) {
  setEnvironment("SUN_MOON_CACHE", "");
  unsetenv("SUN_MOON_CACHE");
  setEnvironment("HOME", (dir / "home").string());
  std::filesystem::current_path(dir);
  EXPECT_EQ(sun::moon_bundling::MoonCache::defaultCacheDir(),
            dir / ".sun/cache/moons");
}

/** Checks SSH, HTTPS, commit, tag, and branch declarations without fetching. */
TEST_F(GitSourceTest, ParsesGitVersionsAndKeepsOutputInProject) {
  for (const auto& url : {"git@example.com:team/library.git",
                          "ssh://git@example.com:2222/team/library.git",
                          "https://example.com/library.git"}) {
    for (const auto& version : {"release/stable", "v1.2.3",
                                "0123456789abcdef0123456789abcdef01234567"}) {
      write(dir / "sun-config.json",
            std::string("{\"entrypoints\":[{\"type\":\"library\",\"git\":\"") +
                url + "\",\"version\":\"" + version +
                "\",\"path\":\"src/lib.sun\","
                "\"name\":\"lib\"}]}");
      const auto config =
          sun::driver::SunConfig::loadFile(dir / "sun-config.json");
      ASSERT_EQ(config.entrypoints.size(), 1u);
      EXPECT_EQ(config.entrypoints[0].path, "src/lib.sun");
      EXPECT_EQ(config.entrypoints[0].name, "lib");
      EXPECT_EQ(config.entrypoints[0].version, version);
    }
  }
  EXPECT_FALSE(std::filesystem::exists(dir / "calls"));
}

/** Rejects invalid revisions, escaping paths, and executable source entries. */
TEST_F(GitSourceTest, RejectsInvalidDeclarations) {
  for (const auto& version : {"", "--upload-pack=bad", "main~1", "bad\nname"}) {
    auto value = entry();
    value.version = version;
    EXPECT_THROW(sun::driver::validateGitSource(value), sun::support::SunError);
  }
  for (const auto& path : {"../lib.sun", "/lib.sun", "src/../../lib.sun"}) {
    auto value = entry();
    value.path = path;
    EXPECT_THROW(sun::driver::validateGitSource(value), sun::support::SunError);
  }
  auto value = entry();
  value.type = sun::driver::ConfigEntrypoint::Type::Binary;
  EXPECT_THROW(sun::driver::validateGitSource(value), sun::support::SunError);
}

/** Cached checkouts work offline and configuration cannot leak from cache
 * parents. */
TEST_F(GitSourceTest, ReusesCheckoutAndStopsConfigDiscovery) {
  const auto source = sun::driver::resolveGitEntrypoint(entry());
  ASSERT_TRUE(std::filesystem::is_regular_file(source));
  const auto calls = std::filesystem::file_size(dir / "calls");
  write(dir / "fail", "fail");
  EXPECT_EQ(sun::driver::resolveGitEntrypoint(entry()), source);
  EXPECT_EQ(std::filesystem::file_size(dir / "calls"), calls);
  write(dir / "cache/sun-config.json",
        "{\"path_variables\":{\"BAD\":\"outside\"}}");
  EXPECT_FALSE(sun::driver::SunConfig::findFrom(
      std::filesystem::path(source).parent_path()));
  auto changed = entry();
  changed.version = "v2";
  EXPECT_THROW(sun::driver::resolveGitEntrypoint(changed),
               sun::support::SunError);
}

/** A failed fetch leaves no usable cache and a subsequent build can retry. */
TEST_F(GitSourceTest, RetriesAfterFailureAndRejectsSymlinkEscape) {
  write(dir / "fail", "fail");
  EXPECT_THROW(sun::driver::resolveGitEntrypoint(entry()),
               sun::support::SunError);
  for (const auto& cached :
       std::filesystem::directory_iterator(dir / "cache")) {
    EXPECT_TRUE(std::filesystem::is_empty(cached.path()));
  }
  std::filesystem::remove(dir / "fail");
  const auto source = sun::driver::resolveGitEntrypoint(entry());
  write(dir / "outside.sun", "outside");
  std::filesystem::remove(source);
  std::filesystem::create_symlink(dir / "outside.sun", source);
  EXPECT_THROW(sun::driver::resolveGitEntrypoint(entry()),
               sun::support::SunError);
}

/** Explicit refresh replaces the selected snapshot and preserves offline
 * fallback. */
TEST_F(GitSourceTest, RefreshesOnlyWhenRequestedAndPreservesOldCheckout) {
  const auto first = sun::driver::resolveGitEntrypoint(entry());
  const auto second = sun::driver::resolveGitEntrypoint(entry(), true);
  EXPECT_NE(first, second);
  EXPECT_TRUE(std::filesystem::is_regular_file(first));
  EXPECT_EQ(sun::driver::resolveGitEntrypoint(entry()), second);
  write(dir / "fail", "fail");
  EXPECT_THROW(sun::driver::resolveGitEntrypoint(entry(), true),
               sun::support::SunError);
  EXPECT_EQ(sun::driver::resolveGitEntrypoint(entry()), second);
}

/** Config builds preserve selected targets, output locations, and skip
 * behavior. */
TEST_F(GitSourceTest,
       BuildsConfiguredGitLibraryAndRefreshesWithoutRecompiling) {
  write(dir / "sun-config.json", R"({
    "root": true,
    "target": {"aarch64-linux-gnu": {
      "sun_path": ["build"],
      "path_variables": {"EXTRAS": "extras"},
      "entrypoints": [{"git": "ssh://git@example.com/team/library.git",
        "version": "v1", "path": "src/lib.sun", "type": "library",
        "name": "lib"}]
    }}
  })");
  write(dir / "repo-config",
        R"({"root":true,"path_variables":{"EXTRAS":"missing"}})");
  write(dir / "manifest",
        "manifest { source_files: [\"$EXTRAS/helper.sun\"] }\n");
  write(dir / "extras/helper.sun",
        "/** Project-provided source. */ public module helper {}\n");
  sun::cli::BuildRunOptions options;
  options.inputFiles = {(dir / "sun-config.json").string()};
  options.targetTriple = "aarch64-linux-gnu";
  options.noTest = true;
  ASSERT_EQ(sun::cli::runConfigBuildCommand(options), 0);
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json",
                                            options.targetTriple);
  auto configured = sun::driver::configureBuildPaths(
      graph, sun::driver::SunConfig::loadFile(dir / "sun-config.json",
                                              options.targetTriple));
  const auto output =
      std::filesystem::path(configured.pathVariables.at("SUN_BUILD")) /
      "lib.moon";
  ASSERT_TRUE(std::filesystem::is_regular_file(output));
  const auto hash = sun::driver::readMoonInputHash(output.string());
  const auto timestamp = std::filesystem::last_write_time(output);
  options.refreshSources = true;
  ASSERT_EQ(sun::cli::runConfigBuildCommand(options), 0);
  EXPECT_EQ(sun::driver::readMoonInputHash(output.string()), hash);
  EXPECT_EQ(std::filesystem::last_write_time(output), timestamp);
  write(dir / "answer", "43");
  ASSERT_EQ(sun::cli::runConfigBuildCommand(options), 0);
  EXPECT_NE(sun::driver::readMoonInputHash(output.string()), hash);
}

/** Git source builds reuse existing hashes and rebuild for changed settings. */
TEST_F(GitSourceTest, ReusesBundleUntilBuildSettingsChange) {
  const auto source = sun::driver::resolveGitEntrypoint(entry());
  const auto output = dir / "lib.moon";
  sun::moon_bundling::MoonBuildOptions options;
  options.targetTriple = "x86_64-linux-gnu";
  EXPECT_FALSE(
      sun::moon_bundling::MoonBuilder::build(source, output, options).upToDate);
  EXPECT_TRUE(
      sun::moon_bundling::MoonBuilder::build(source, output, options).upToDate);
  options.targetTriple = "aarch64-linux-gnu";
  EXPECT_FALSE(
      sun::moon_bundling::MoonBuilder::build(source, output, options).upToDate);
  EXPECT_TRUE(
      sun::moon_bundling::MoonBuilder::build(source, output, options).upToDate);
  options.optimize = false;
  EXPECT_FALSE(
      sun::moon_bundling::MoonBuilder::build(source, output, options).upToDate);
  options.forceRebuild = true;
  EXPECT_FALSE(
      sun::moon_bundling::MoonBuilder::build(source, output, options).upToDate);
}

/** Source descriptors accept all supported SSH forms without fetching. */
TEST_F(GitSourceTest, ParsesConfigDependenciesWithoutFetching) {
  for (const auto& url : {"git@example.com:team/library.git",
                          "ssh://git@example.com:2222/team/library.git",
                          "https://example.com/library.git"}) {
    write(dir / "sun-config.json",
          std::string(
              R"({"root":true,"dependencies":[{"package":"LIB","git":")") +
              url + R"(","version":"v1","entrypoint":"library"}]})");
    auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
    EXPECT_EQ(config.dependencies.at("LIB").selections.front().git, url);
    EXPECT_EQ(config.dependencies.at("LIB").selections.front().config,
              "sun-config.json");
    EXPECT_EQ(config.dependencies.at("LIB").selections.front().entrypoint,
              "library");
  }
  EXPECT_FALSE(std::filesystem::exists(dir / "calls"));
  for (
      const auto& descriptor :
      {R"({"git":"--bad","version":"v1"})",
       R"({"git":"git@example.com:lib.git"})",
       R"({"git":"git@example.com:lib.git","version":"v1","config":"../escape.json"})",
       R"({"git":"git@example.com:lib.git","version":"v1","moon":{}})"}) {
    write(dir / "sun-config.json",
          std::string(R"({"dependencies":[)") + descriptor + "]}");
    EXPECT_THROW(sun::driver::SunConfig::loadFile(dir / "sun-config.json"),
                 sun::support::SunError);
  }
}

/** A fresh config dependency uses its selected config and skips other products.
 */
TEST_F(GitSourceTest, BuildsSelectedLibraryFromCustomConfigAndImportsIt) {
  write(
      dir / "sun-config.json",
      R"({"root":true,"sun_path":["local"],"dependencies":[{"package":"LIB","git":"git@example.com:team/library.git","version":"v1","config":"library-config.json","entrypoint":"api"}]})");
  write(dir / "repos/library.git/library-config.json",
        R"({"root":true,"sun_path":["vendor"],
    "path_variables":{"SOURCES":"src"},"entrypoints":[
      {"name":"api","path":"$SOURCES/lib.sun","type":"library"},
      {"name":"private_api","path":"missing.sun","type":"library"},
      {"path":"also-missing.sun","type":"binary"}]})");
  write(dir / "repos/library.git/sun-config.json", "invalid default config");
  for (const auto& [folder, answer] :
       std::vector<std::pair<std::string, std::string>>{
           {"local", "42"}, {"repos/library.git/vendor", "7"}}) {
    write(dir / "provider.sun",
          "/** Library provider. */ public module provider { "
          "/** Returns the provider's answer. */ public function answer() i32 "
          "{ return " +
              answer + "; } }\n");
    std::filesystem::create_directories(dir / folder);
    sun::moon_bundling::MoonBuilder::build((dir / "provider.sun").string(),
                                           dir / folder / "provider.moon");
  }
  write(dir / "repos/library.git/src/lib.sun",
        "/** Fixture library. */ public module fixture {\n"
        "/** Uses the selected provider. */ public function answer() i32 { "
        "return provider.answer(); }\n"
        "}\nmanifest { libraries: [\"provider.moon\"], test_files: "
        "[\"missing_test.sun\"] }\n");
  write(dir / "main.sun",
        "manifest { libraries: [\"$LIB/lib/api.moon\"] }\n"
        "/** Returns the imported answer. */ function main() i32 { return "
        "fixture.answer(); }\n");
  sun::driver::GitDependencyBuildScope scope(false, true);
  auto driver = sun::driver::Driver::createForJIT("dependency_test");
  EXPECT_EQ(std::get<int32_t>(driver->executeFile((dir / "main.sun").string())),
            42);
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  const auto output = sun::driver::resolveConfigDependency(config, "LIB");
  ASSERT_TRUE(std::filesystem::is_regular_file(output / "lib/api.moon"));
  const auto timestamp =
      std::filesystem::last_write_time(output / "lib/api.moon");
  const auto calls = std::filesystem::file_size(dir / "calls");
  write(dir / "fail", "offline");
  EXPECT_EQ(sun::driver::resolveConfigDependency(config, "LIB"), output);
  EXPECT_EQ(std::filesystem::file_size(dir / "calls"), calls);
  EXPECT_EQ(std::filesystem::last_write_time(output / "lib/api.moon"),
            timestamp);
  EXPECT_FALSE(std::filesystem::exists(output / "lib/api_test"));
}

/** Selection errors and cycles fail clearly, then allow a corrected build. */
TEST_F(GitSourceTest, RejectsAmbiguousMissingAndCyclicSelections) {
  write(dir / "repo-config", R"({"root":true,"entrypoints":[
    {"name":"first","path":"src/lib.sun","type":"library"},
    {"name":"second","path":"src/lib.sun","type":"library"},
    {"name":"app","path":"app.sun","type":"binary"}]})");
  write(
      dir / "sun-config.json",
      R"({"root":true,"dependencies":[{"package":"LIB","git":"git@example.com:team/library.git","version":"v1"}]})");
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  sun::driver::GitDependencyBuildScope scope(false, true);
  for (const auto& selection : {"", "missing", "app"}) {
    config.dependencies.at("LIB").selections.front().entrypoint = selection;
    EXPECT_THROW(sun::driver::resolveConfigDependency(config, "LIB"),
                 sun::support::SunError);
  }
  config.dependencies.at("LIB").selections.front().entrypoint = "first";
  EXPECT_TRUE(std::filesystem::exists(
      sun::driver::resolveConfigDependency(config, "LIB") / "lib/first.moon"));

  write(
      dir / "repos/cycle.git/sun-config.json",
      R"({"root":true,"dependencies":[{"package":"SELF","git":"git@example.com:cycle.git","version":"v1"}],"entrypoints":[{"path":"lib.sun","type":"library"}]})");
  write(dir / "repos/cycle.git/lib.sun",
        "manifest { libraries: [\"$SELF/lib.moon\"] }\n");
  config.dependencies.at("LIB").selections.front().git =
      "git@example.com:cycle.git";
  config.dependencies.at("LIB").selections.front().entrypoint.clear();
  testing::internal::CaptureStderr();
  EXPECT_THROW(sun::driver::resolveConfigDependency(config, "LIB"),
               sun::support::SunError);
  EXPECT_NE(testing::internal::GetCapturedStderr().find(
                "cyclic Git library dependency"),
            std::string::npos);
  config.dependencies.at("LIB").selections.front().git =
      "git@example.com:team/library.git";
  config.dependencies.at("LIB").selections.front().entrypoint = "first";
  EXPECT_TRUE(std::filesystem::exists(
      sun::driver::resolveConfigDependency(config, "LIB") / "lib/first.moon"));
}

/** Transitive libraries inherit the target, build settings, and refresh scope.
 */
TEST_F(GitSourceTest, BuildsTransitiveDependenciesAndRefreshesOnce) {
  write(
      dir / "sun-config.json",
      R"({"root":true,"dependencies":[{"package":"LIB","git":"ssh://git@example.com/top.git","version":"main"}]})");
  write(
      dir / "repos/top.git/sun-config.json",
      R"({"root":true,"dependencies":[{"package":"LEAF","git":"git@example.com:leaf.git","version":"v1"}],"entrypoints":[{"path":"top.sun","type":"library"}]})");
  write(dir / "repos/top.git/top.sun",
        "manifest { libraries: [\"$LEAF/lib/leaf.moon\"] }\n"
        "/** Top library. */ public module top { /** Uses its dependency. */\n"
        "public function answer() i32 { return leaf.answer(); } }\n");
  write(dir / "repos/leaf.git/sun-config.json", R"({"root":true,"entrypoints":[
    {"path":"leaf.sun","type":"library"}]})");
  write(dir / "repos/leaf.git/leaf.sun",
        "/** Leaf library. */ public module leaf {\n"
        "/** Returns an answer. */ public function answer() i32 { return 42; } "
        "}\n");
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json",
                                                 "aarch64-linux-gnu");
  std::filesystem::path first;
  {
    sun::driver::GitDependencyBuildScope scope(false, false, false, true);
    first = sun::driver::resolveConfigDependency(config, "LIB");
    EXPECT_NE(first.string().find("aarch64"), std::string::npos);
    EXPECT_NE(first.string().find("unoptimized"), std::string::npos);
    const auto calls = std::filesystem::file_size(dir / "calls");
    EXPECT_EQ(sun::driver::resolveConfigDependency(config, "LIB"), first);
    EXPECT_EQ(std::filesystem::file_size(dir / "calls"), calls);
  }
  {
    sun::driver::GitDependencyBuildScope scope(false, true, false, true);
    EXPECT_NE(sun::driver::resolveConfigDependency(config, "LIB"), first);
    EXPECT_TRUE(std::filesystem::exists(first / "lib/top.moon"));
  }
}

/** Unavailable target descriptors stay lazy and cannot silently use host
 * sources. */
TEST_F(GitSourceTest, KeepsUnavailableSourceDependenciesLazy) {
  write(
      dir / "sun-config.json",
      R"({"root":true,"dependencies":[{"target":{"aarch64-linux-gnu":{"package":"LIB","git":"git@example.com:library.git","version":"v1"}}}]})");
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json",
                                                 "x86_64-linux-gnu");
  EXPECT_EQ(config.dependencies.count("LIB"), 0u);
  EXPECT_THROW(sun::driver::resolveConfigDependency(config, "LIB"),
               sun::support::SunError);
  EXPECT_FALSE(std::filesystem::exists(dir / "calls"));
}

/** Config-file containment also applies to the selected library's source. */
TEST_F(GitSourceTest, RejectsEscapingSelectedLibrary) {
  write(
      dir / "sun-config.json",
      R"({"root":true,"dependencies":[{"package":"LIB","git":"git@example.com:library.git","version":"v1"}]})");
  write(dir / "repo-config", R"({"root":true,"entrypoints":[
    {"path":"../../outside.sun","type":"library"}]})");
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  EXPECT_THROW(sun::driver::resolveConfigDependency(config, "LIB"),
               sun::support::SunError);
}

/** Direct source dependencies need no repository config and use consumer
 * settings. */
TEST_F(GitSourceTest, BuildsDirectSourceWithConsumerConfiguration) {
  write(
      dir / "sun-config.json",
      R"({"root":true,"path_variables":{"EXTRAS":"extras"},"dependencies":[{"package":"LIB","git":"git@example.com:team/library.git","version":"v1","path":"src/lib.sun"}]})");
  write(dir / "repo-config", "not a valid project config");
  write(dir / "manifest",
        "manifest { source_files: [\"$EXTRAS/helper.sun\"] }\n");
  write(dir / "extras/helper.sun",
        "/** Consumer-provided source. */ public module helper {}\n");
  write(dir / "main.sun",
        "manifest { libraries: [\"$LIB/lib/lib.moon\"] }\n"
        "/** Calls the dependency. */ function main() i32 { return "
        "fixture.answer(); }\n");
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  EXPECT_EQ(config.dependencies.at("LIB").selections.front().path,
            "src/lib.sun");
  EXPECT_FALSE(std::filesystem::exists(dir / "calls"));
  sun::driver::GitDependencyBuildScope scope(false, true);
  auto driver = sun::driver::Driver::createForJIT("direct_dependency_test");
  EXPECT_EQ(std::get<int32_t>(driver->executeFile((dir / "main.sun").string())),
            42);
  auto output = sun::driver::resolveConfigDependency(config, "LIB");
  ASSERT_TRUE(std::filesystem::is_regular_file(output / "lib/lib.moon"));
  const auto timestamp =
      std::filesystem::last_write_time(output / "lib/lib.moon");
  write(dir / "fail", "offline");
  EXPECT_EQ(sun::driver::resolveConfigDependency(config, "LIB"), output);
  EXPECT_EQ(std::filesystem::last_write_time(output / "lib/lib.moon"),
            timestamp);
}

/** A source selection cannot also select a config, and must stay in the
 * repository. */
TEST_F(GitSourceTest, RejectsConflictingOrEscapingDirectSelections) {
  for (const auto& fields :
       {R"("path":"src/lib.sun","config":"sun-config.json")",
        R"("path":"src/lib.sun","entrypoint":"library")",
        R"("path":"../lib.sun")", R"("path":"/lib.sun")", R"("path":"")"}) {
    write(dir / "sun-config.json",
          std::string(R"({"dependencies":[{"package":"LIB",
      "git":"ssh://git@example.com/library.git","version":"v1",)") +
              fields + "}]}");
    EXPECT_THROW(sun::driver::SunConfig::loadFile(dir / "sun-config.json"),
                 sun::support::SunError);
  }
  EXPECT_FALSE(std::filesystem::exists(dir / "calls"));
}

/** Preparation generates sources before prerequisites and preserves installs on
 * failure. */
TEST_F(GitSourceTest, BuildsGeneratedPrerequisiteAndTracksInputs) {
  write(dir / "sun-config.json", R"({"dependencies":[{"package":"LIB",
    "git":"git@example.com:generated.git","version":"v1","entrypoint":"api"}]})");
  write(dir / "repos/generated.git/sun-config.json",
        R"({"package_id":"example/generated",
    "steps":[{"name":"generate","command":["/bin/cp","input.sun","${SUN_BUILD}/generated.sun"],
      "inputs":["input.sun"],"outputs":["${SUN_BUILD}/generated.sun"]}],
    "entrypoints":[
      {"name":"generated","path":"${SUN_BUILD}/generated.sun","type":"library","steps":["generate"]},
      {"name":"api","path":"api.sun","type":"library","requires":["generated"]}]})");
  write(dir / "repos/generated.git/input.sun",
        "/** Generated module. */ public module generated { /** Returns a "
        "value. */ public function value() i32 { return 42; } }");
  write(dir / "repos/generated.git/api.sun",
        "manifest { libraries: [\"${SUN_BUILD}/generated.moon\"] }\n"
        "/** Public API. */ public module api { /** Reads generated code. */ "
        "public function value() i32 { return generated.value(); } }");
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  auto installed = sun::driver::resolveConfigDependency(config, "LIB");
  const auto artifact = installed / "lib/api.moon";
  ASSERT_TRUE(std::filesystem::exists(artifact));
  const auto timestamp = std::filesystem::last_write_time(artifact);
  EXPECT_EQ(sun::driver::resolveConfigDependency(config, "LIB"), installed);
  EXPECT_EQ(std::filesystem::last_write_time(artifact), timestamp);
  auto source = entry();
  source.git = "git@example.com:generated.git";
  source.version = "v1";
  source.path = "input.sun";
  const auto input = sun::driver::resolveGitEntrypoint(source);
  const auto hash = sun::driver::packageFileHash(artifact);
  write(input,
        "/** Changed module. */ public module generated { /** Returns a value. "
        "*/ public function value() i32 { return 43; } }");
  sun::driver::resolveConfigDependency(config, "LIB");
  EXPECT_NE(sun::driver::packageFileHash(artifact), hash);
  const auto updated = sun::driver::packageFileHash(artifact);
  std::filesystem::remove(input);
  EXPECT_THROW(sun::driver::resolveConfigDependency(config, "LIB"),
               sun::support::SunError);
  EXPECT_EQ(sun::driver::packageFileHash(artifact), updated);
  EXPECT_FALSE(std::filesystem::exists(
      std::filesystem::path(input).parent_path() / ".sun"));
}

/** Native preparation runs before the Moon loader embeds its archive. */
TEST_F(GitSourceTest, BuildsAndEmbedsNativeArchiveFromEmptyCache) {
  write(dir / "sun-config.json", R"({"dependencies":[{"package":"LIB",
    "git":"git@example.com:native.git","version":"v1","entrypoint":"api"}]})");
  write(dir / "repos/native.git/sun-config.json",
        R"({"package_id":"example/native",
    "steps":[
      {"name":"compile","command":["/usr/bin/cc","-c","native.c","-o","${SUN_BUILD}/native.o"],
       "inputs":["native.c"],"outputs":["${SUN_BUILD}/native.o"]},
      {"name":"native","requires":["compile"],"command":["/usr/bin/ar","rcs","${SUN_BUILD}/native.a","${SUN_BUILD}/native.o"],
       "inputs":["${SUN_BUILD}/native.o"],"outputs":["${SUN_BUILD}/native.a"]}],
    "entrypoints":[{"name":"api","path":"api.sun","type":"library","steps":["native"]}]})");
  write(dir / "repos/native.git/native.c",
        "/** Supplies a native archive member. */ int native_value(void) { "
        "return 42; }\n");
  write(
      dir / "repos/native.git/api.sun",
      "manifest { archives: [\"${SUN_BUILD}/native.a\"] }\n"
      "/** Native fixture. */ public module native_fixture {\n"
      "/** Calls the C archive. */ extern \"C\" function native_value() i32;\n"
      "/** Exposes the native value. */ public function value() i32 { return "
      "unsafe { native_value(); }; } }");
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  auto installed = sun::driver::resolveConfigDependency(config, "LIB");
  ASSERT_TRUE(std::filesystem::exists(installed / "lib/api.moon"));
  write(dir / "main.sun",
        "manifest { libraries: [\"$LIB/lib/api.moon\"] }\n/** Consumer. */ "
        "function main() i32 { return native_fixture.value(); }");
  auto driver = sun::driver::Driver::createForAOT("git_native_consumer");
  driver->compileFile((dir / "main.sun").string());
  EXPECT_EQ(driver->getNativeArchivePaths().size(), 1u);
}

/** Custom scripts receive build context and stage their declared resources. */
TEST_F(GitSourceTest, RunsCustomScriptAndStagesResources) {
  write(dir / "sun-config.json",
        R"({"dependencies":[{"package":"TOOL",
    "git":"git@example.com:custom.git","version":"v1","entrypoint":"tool"}]})");
  write(dir / "repos/custom.git/sun-config.json",
        R"({"package_id":"example/tools",
    "entrypoints":[{"name":"tool","path":"tool.sun","type":"binary","build":["build.py"],
      "resources":[{"path":"data","destination":"tool.data"}]}]})");
  write(dir / "repos/custom.git/tool.sun", "manifest { id: \"example/tool\" }");
  write(dir / "repos/custom.git/data/value.txt", "resource");
  write(dir / "repos/custom.git/build.py", R"PY(#!/usr/bin/python3
"""Validate custom context and emit a host generator."""
import json, os, pathlib
context = json.loads(pathlib.Path(os.environ['SUN_BUILD_CONTEXT']).read_text())
assert context['output'] == os.environ['SUN_OUTPUT']
assert context['source'] == os.environ['SUN_SOURCE']
pathlib.Path(os.environ['SUN_OUTPUT']).write_text('#!/bin/sh\n/bin/cp "$1" "$2"\n')
)PY");
  std::filesystem::permissions(dir / "repos/custom.git/build.py",
                               std::filesystem::perms::owner_all);
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  auto installed = sun::driver::resolveConfigDependency(config, "TOOL");
  EXPECT_TRUE(std::filesystem::exists(installed / "bin/tool"));
  EXPECT_TRUE(std::filesystem::exists(installed / "bin/tool.data/value.txt"));
}
/** Git generators use host settings while their consumer builds for another
 * target. */
TEST_F(GitSourceTest, UsesHostGitGeneratorForForeignTarget) {
  const auto host = llvm::sys::getDefaultTargetTriple();
  const auto target =
      sun::driver::configTargetKey(host) == "x86_64-unknown-linux-gnu"
          ? "aarch64-linux-gnu"
          : "x86_64-linux-gnu";
  write(dir / "sun-config.json", R"({"package_id":"example/consumer",
    "dependencies":[{"package":"TOOLS","binary":"example/tool","git":"git@example.com:tools.git","version":"v1"}],
    "steps":[{"name":"generate","command":["${TOOLS}/bin/tool","input.sun","${SUN_BUILD}/generated.sun"],
      "inputs":["input.sun"],"outputs":["${SUN_BUILD}/generated.sun"]}],
    "entrypoints":[{"name":"consumer","path":"${SUN_BUILD}/generated.sun","type":"library","export":true,"steps":["generate"]}]})");
  setEnvironment("EXPECTED_HOST",
                 sun::driver::SunConfig::loadFile(dir / "sun-config.json", host)
                     .targetTriple);
  write(dir / "input.sun",
        "/** Generated consumer. */ public module consumer {}");
  write(dir / "repos/tools.git/sun-config.json",
        R"({"package_id":"example/tools",
    "entrypoints":[{"name":"tool","path":"tool.sun","type":"binary","build":["build.py"]}]})");
  write(dir / "repos/tools.git/tool.sun", "manifest { id: \"example/tool\" }");
  write(dir / "repos/tools.git/build.py", R"PY(#!/usr/bin/python3
"""Emit a generator only when built for the requested host."""
import os, pathlib
assert os.environ['SUN_TARGET'] == os.environ['EXPECTED_HOST']
pathlib.Path(os.environ['SUN_OUTPUT']).write_text('#!/bin/sh\n/bin/cp "$1" "$2"\n')
)PY");
  std::filesystem::permissions(dir / "repos/tools.git/build.py",
                               std::filesystem::perms::owner_all);
  sun::cli::BuildRunOptions options;
  options.inputFiles = {(dir / "sun-config.json").string()};
  options.targetTriple = target;
  options.noTest = true;
  EXPECT_EQ(sun::cli::runConfigBuildCommand(options), 0);
}
/** Distinct Git selections do not publish each other's accumulated products. */
TEST_F(GitSourceTest, CombinesSelectionsFromOneRepository) {
  write(dir / "sun-config.json", R"({"dependencies":[
    {"package":"LIB","library":"example/first","git":"git@example.com:pair.git","version":"v1"},
    {"package":"LIB","library":"example/second","git":"git@example.com:pair.git","version":"v1"}]})");
  write(
      dir / "repos/pair.git/sun-config.json",
      R"({"package_id":"example/pair","exports":[{"path":"NOTICE","destination":"share/NOTICE"}],"entrypoints":[
    {"name":"first","path":"first.sun","type":"library"},
    {"name":"second","path":"second.sun","type":"library"}]})");
  write(dir / "repos/pair.git/NOTICE", "shared notice");
  write(dir / "repos/pair.git/first.sun", "manifest { id: \"example/first\" }");
  write(dir / "repos/pair.git/second.sun",
        "manifest { id: \"example/second\" }");
  auto graph = sun::driver::loadConfigGraph(dir / "sun-config.json");
  sun::driver::ConfigGraphScope scope(graph);
  auto config = sun::driver::SunConfig::loadFile(dir / "sun-config.json");
  const auto installed = sun::driver::resolveConfigDependency(config, "LIB");
  EXPECT_TRUE(std::filesystem::exists(installed / "lib/first.moon"));
  EXPECT_TRUE(std::filesystem::exists(installed / "lib/second.moon"));
  EXPECT_TRUE(std::filesystem::exists(installed / "share/NOTICE"));
}
}  // namespace
