#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>

#include "cli/config_build_command.h"
#include "driver/build_record.h"
#include "driver/git_source.h"
#include "driver/manifest_processor.h"
#include "driver/sun_config.h"
#include "moon_bundling/moon_builder.h"
#include "support/error.h"
#include "support/sun_path.h"

/** Keeps Git source fixtures private to this test file. */
namespace {
/** Exercises Git orchestration with a fake executable, without network access.
 */
class GitSourceTest : public testing::Test {
 protected:
  std::filesystem::path dir;
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
elif args[2] == 'checkout':
    assert args[3:] == ['--quiet', '--detach', 'FETCH_HEAD']
else:
    raise AssertionError(args)
if 'checkout' in args:
    checkout = pathlib.Path(args[args.index('-C') + 1])
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
    setEnvironment("PATH", (dir / "bin").string());
  }

  /** Restores process settings after each test. */
  void TearDown() override {
    sun::support::SunPath::extraPaths() = searchPaths;
    sun::driver::ManifestProcessor::clearPathVariables();
    for (const auto& [name, value] : environment) {
      if (value)
        setenv(name.c_str(), value->c_str(), 1);
      else
        unsetenv(name.c_str());
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
                "\"output_name\":\"build/lib\"}]}");
      const auto config =
          sun::driver::SunConfig::loadFile(dir / "sun-config.json");
      ASSERT_EQ(config.entrypoints.size(), 1u);
      EXPECT_EQ(config.entrypoints[0].path, "src/lib.sun");
      EXPECT_EQ(config.entrypoints[0].outputName, (dir / "build/lib").string());
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
  for (const auto& cached : std::filesystem::directory_iterator(dir / "cache"))
    EXPECT_TRUE(std::filesystem::is_empty(cached.path()));
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
        "output_name": "build/lib"}]
    }}
  })");
  write(dir / "repo-config", R"({"root":true,"path_variables":{"EXTRAS":"missing"}})");
  write(dir / "manifest",
        "manifest { source_files: [\"$EXTRAS/helper.sun\"] }\n");
  write(dir / "extras/helper.sun",
        "/** Project-provided source. */ public module helper {}\n");
  sun::cli::BuildRunOptions options;
  options.inputFiles = {(dir / "sun-config.json").string()};
  options.targetTriple = "aarch64-linux-gnu";
  options.noTest = true;
  ASSERT_EQ(sun::cli::runConfigBuildCommand(options), 0);
  const auto output = dir / "build/lib.moon";
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
}  // namespace
