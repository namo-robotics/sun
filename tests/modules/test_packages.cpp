#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "driver/manifest_processor.h"
#include "driver/package.h"
#include "driver/sun_config.h"
#include "support/error.h"

/** Keeps package fixtures isolated from other test suites. */
namespace {
/** Shortens fixture filesystem operations. */
namespace fs = std::filesystem;
using sun::driver::ManifestProcessor;
using sun::driver::SunConfig;
using sun::support::SunError;

/** Writes a fixture, creating its containing directory. */
void put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

/** Owns a fresh fixture directory and restores the cache environment. */
class Packages : public testing::Test {
 protected:
  fs::path dir;
  std::string previousCache;
  bool hadCache = false;
  /** Creates fixtures beneath the workspace temporary directory. */
  void SetUp() override {
    dir = fs::current_path() / "tmp/package-tests" /
          testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (auto* value = std::getenv("SUN_DEPENDENCY_CACHE")) {
      hadCache = true;
      previousCache = value;
    }
    setenv("SUN_DEPENDENCY_CACHE", (dir / "cache").c_str(), 1);
    ManifestProcessor::clearPathVariables();
  }
  /** Restores process settings after every test, including failed assertions.
   */
  void TearDown() override {
    if (hadCache)
      setenv("SUN_DEPENDENCY_CACHE", previousCache.c_str(), 1);
    else
      unsetenv("SUN_DEPENDENCY_CACHE");
    ManifestProcessor::clearPathVariables();
  }
  /** Parses a fixture configuration for an explicit target. */
  SunConfig config(const std::string& text,
                   const std::string& target = "x86_64-linux-gnu") {
    put(dir / "sun-config.json", text);
    return SunConfig::loadFile(dir / "sun-config.json", target);
  }
};

/** Target replacements apply to individual fields without repeating products.
 */
TEST_F(Packages, PerSettingTargets) {
  auto cfg = config(R"({
    "sun_path": {"default":["build"],"target":{"aarch64-linux-gnu":["arm"]}},
    "path_variables":{"LIBS":{"default":"libs","target":{"aarch64-linux-gnu":"arm-libs"}}},
    "entrypoints":[
      {"name":"app","path":"main.sun","output_name":{"default":"app","target":{"aarch64-linux-gnu":"arm-app"}}},
      {"name":"helper","enabled":{"default":false,"target":{"aarch64-linux-gnu":true}},"path":{"target":{"aarch64-linux-gnu":"helper.sun"}}}
    ]})");
  ASSERT_EQ(cfg.entrypoints.size(), 1u);
  EXPECT_EQ(cfg.sunPath, std::vector<std::string>{(dir / "build").string()});
  auto arm =
      SunConfig::loadFile(dir / "sun-config.json", "arm64-unknown-linux-gnu");
  ASSERT_EQ(arm.entrypoints.size(), 2u);
  EXPECT_EQ(arm.entrypoints[0].outputName, (dir / "arm-app").string());
  EXPECT_EQ(arm.pathVariables.at("LIBS"), (dir / "arm-libs").string());
  EXPECT_EQ(arm.sunPath.size(), 1u);
}

/** Invalid inactive branches and overlapping override styles are rejected. */
TEST_F(Packages, TargetValidation) {
  EXPECT_THROW(
      config(R"({"sun_path":{"target":{"aarch64-linux-gnu":["arm"]}}})"),
      SunError);
  EXPECT_THROW(
      config(
          R"({"sun_path":{"default":[],"target":{"aarch64-linux-gnu":42}}})"),
      SunError);
  EXPECT_THROW(
      config(
          R"({"sun_path":{"default":[]},"target":{"aarch64-linux-gnu":{"sun_path":[]}}})"),
      SunError);
  EXPECT_THROW(
      config(
          R"({"sun_path":{"default":[],"target":{"aarch64-linux-gnu":[],"arm64-linux-gnu":[]}}})"),
      SunError);
}

/** Direct Moon inputs use target-separated directories and stable filenames. */
TEST_F(Packages, MoonCacheAndCollision) {
  put(dir / "x64.moon", "x64 bytes");
  put(dir / "arm.moon", "arm bytes");
  auto cfg = config(R"({"dependencies":{"IMAGE_CORE":{"target":{
    "x86_64-linux-gnu":{"moon":{"path":"x64.moon","filename":"image_core.moon"}},
    "aarch64-linux-gnu":{"moon":{"path":"arm.moon","filename":"image_core.moon"}}
  }}}})");
  EXPECT_FALSE(fs::exists(dir / "cache"));
  auto x64 = ManifestProcessor::expandPathVariables(
      "$IMAGE_CORE/image_core.moon", &cfg);
  EXPECT_EQ(sun::driver::packageFileHash(x64),
            sun::driver::packageFileHash(dir / "x64.moon"));
  EXPECT_NE(x64.find(sun::driver::configTargetKey("x86_64-linux-gnu")),
            std::string::npos);
  put(x64, "damaged");
  EXPECT_EQ(ManifestProcessor::expandPathVariables(
                "$IMAGE_CORE/image_core.moon", &cfg),
            x64);
  EXPECT_EQ(sun::driver::packageFileHash(x64),
            sun::driver::packageFileHash(dir / "x64.moon"));
  auto arm = SunConfig::loadFile(dir / "sun-config.json", "aarch64-linux-gnu");
  EXPECT_NE(ManifestProcessor::expandPathVariables(
                "$IMAGE_CORE/image_core.moon", &arm),
            x64);
  ManifestProcessor::setPathVariable("IMAGE_CORE", "/override");
  EXPECT_THROW(ManifestProcessor::expandPathVariables(
                   "$IMAGE_CORE/image_core.moon", &cfg),
               SunError);
}

/** Unused unavailable targets are allowed, and used ones fail clearly. */
TEST_F(Packages, LazyDependencyTargetAndHash) {
  auto cfg = config(R"({"dependencies":{"LIB":{"target":{"aarch64-linux-gnu":{
    "moon":{"path":"absent.moon","filename":"lib.moon"}
  }}}}})");
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "LIB"), SunError);
  EXPECT_THROW(
      config(
          R"({"dependencies":{"LIB":{"moon":{"url":"https://example.com/lib.moon","filename":"lib.moon"}}}})"),
      SunError);
  EXPECT_THROW(
      config(
          R"({"dependencies":{"LIB":{"moon":{"path":"lib.moon","filename":"../lib.moon"}}}})"),
      SunError);
}

/** Multiple outputs and explicit resources survive packaging and cache
 * extraction. */
TEST_F(Packages, RoundTripAndResourceRebuild) {
  put(dir / "app", "binary");
  put(dir / "helpers.moon", "library");
  put(dir / "assets/message.txt", "hello");
  auto cfg = config(R"({"root":true,"entrypoints":[
    {"name":"app","path":"main.sun","resources":[{"path":"assets","destination":"share/app"}]},
    {"name":"helpers","path":"helpers.sun","type":"library"},
    {"name":"disabled","enabled":false}
  ],"packages":[{"name":"tools","entrypoints":["app","helpers","disabled"]}]})");
  std::vector<sun::driver::PackageArtifact> artifacts{
      {"app", (dir / "app").string(), false},
      {"helpers", (dir / "helpers.moon").string(), true}};
  auto plans = sun::driver::planPackages(cfg, artifacts);
  ASSERT_EQ(plans.size(), 1u);
  sun::driver::buildPackages(cfg, plans);
  const auto archive = fs::path(plans[0].output.string() + ".tar.gz");
  const auto firstHash = sun::driver::packageFileHash(archive);
  auto modified = fs::last_write_time(archive);
  sun::driver::buildPackages(cfg, plans);
  EXPECT_EQ(fs::last_write_time(archive), modified);
  fs::remove(archive);
  sun::driver::buildPackages(cfg, plans);
  EXPECT_EQ(sun::driver::packageFileHash(archive), firstHash);
  sun::driver::ConfigDependency dependency;
  dependency.package = true;
  dependency.path = archive;
  cfg.dependencies["TOOLS"] = dependency;
  auto cached = sun::driver::resolveConfigDependency(cfg, "TOOLS");
  EXPECT_TRUE(fs::exists(cached / "lib/helpers.moon"));
  EXPECT_TRUE(fs::exists(cached / "share/app/message.txt"));
  cfg.targetTriple = "aarch64-linux-gnu";
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "TOOLS"), SunError);
  cfg.targetTriple = "x86_64-linux-gnu";
  put(dir / "assets/message.txt", "changed");
  sun::driver::buildPackages(cfg, plans);
  EXPECT_NE(sun::driver::packageFileHash(archive), firstHash);
  fs::remove(dir / "assets/message.txt");
  sun::driver::buildPackages(cfg, plans);
  EXPECT_FALSE(fs::exists(plans[0].output / "share/app/message.txt"));
  EXPECT_EQ(sun::driver::packageFileHash(dir / "app"),
            sun::driver::packageFileHash(plans[0].output / "bin/app"));
}

/** Automatic packages exclude unselected artifacts and detect output conflicts.
 */
TEST_F(Packages, AutomaticAndConflicts) {
  put(dir / "asset", "data");
  put(dir / "app", "binary");
  auto cfg = config(
      R"({"entrypoints":[{"name":"app","path":"main.sun","resources":[{"path":"asset","destination":"share/data"}]}]})");
  std::vector<sun::driver::PackageArtifact> artifacts{
      {"app", (dir / "app").string(), false}};
  auto plans = sun::driver::planPackages(cfg, artifacts);
  ASSERT_EQ(plans.size(), 1u);
  EXPECT_EQ(plans[0].name, "app");
  plans[0].resources[0].destination = "bin/app";
  EXPECT_THROW(sun::driver::buildPackages(cfg, plans), SunError);
  plans[0].resources[0].destination = "../escaped";
  EXPECT_THROW(sun::driver::buildPackages(cfg, plans), SunError);
  fs::create_symlink(dir / "asset", dir / "link");
  cfg.entrypoints[0].resources[0].path = (dir / "link").string();
  EXPECT_THROW(sun::driver::planPackages(cfg, artifacts), SunError);
}

/** Creates a malicious tar fixture without relying on external archive
 * commands. */
void maliciousArchive(const fs::path& path, const std::string& name,
                      bool link) {
  archive* writer = archive_write_new();
  archive_write_set_format_pax_restricted(writer);
  archive_write_add_filter_gzip(writer);
  archive_write_open_filename(writer, path.c_str());
  archive_entry* entry = archive_entry_new();
  archive_entry_set_pathname(entry, name.c_str());
  archive_entry_set_filetype(entry, link ? AE_IFLNK : AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  archive_entry_set_size(entry, 0);
  if (link) archive_entry_set_symlink(entry, "/etc/passwd");
  archive_write_header(writer, entry);
  archive_entry_free(entry);
  archive_write_close(writer);
  archive_write_free(writer);
}

/** Extraction never follows links or writes paths outside its private staging
 * root. */
TEST_F(Packages, UnsafeArchives) {
  auto cfg =
      config(R"({"dependencies":{"BAD":{"package":{"path":"bad.tar.gz"}}}})");
  maliciousArchive(dir / "bad.tar.gz", "../escaped", false);
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "BAD"), SunError);
  maliciousArchive(dir / "bad.tar.gz", "link", true);
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "BAD"), SunError);
  EXPECT_FALSE(fs::exists(dir / "escaped"));
}
}  // namespace
