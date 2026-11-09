#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>
#include <llvm/TargetParser/Host.h>

#include <filesystem>
#include <fstream>

#include "driver/manifest_processor.h"
#include "driver/build_graph.h"
#include "driver/package.h"
#include "driver/sun_config.h"
#include "moon_bundling/moon_builder.h"
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
    if (hadCache) {
      setenv("SUN_DEPENDENCY_CACHE", previousCache.c_str(), 1);
    } else {
      unsetenv("SUN_DEPENDENCY_CACHE");
    }
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
      {"name":"app","path":"main.sun"},
      {"name":"helper","enabled":{"default":false,"target":{"aarch64-linux-gnu":true}},"path":{"target":{"aarch64-linux-gnu":"helper.sun"}}}
    ]})");
  ASSERT_EQ(cfg.entrypoints.size(), 1u);
  EXPECT_EQ(cfg.sunPath, std::vector<std::string>{(dir / "build").string()});
  auto arm =
      SunConfig::loadFile(dir / "sun-config.json", "arm64-unknown-linux-gnu");
  ASSERT_EQ(arm.entrypoints.size(), 2u);
  EXPECT_EQ(arm.entrypoints[0].name, "app");
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
  put(dir / "image.sun",
      "manifest { id: \"image_core\" }\n"
      "/** Fixture library. */ public module image_core { /** Returns a value. "
      "*/ public function value() i32 { return 1; } }\n");
  sun::moon_bundling::MoonBuildOptions options;
  options.targetTriple = "x86_64-linux-gnu";
  sun::moon_bundling::MoonBuilder::build((dir / "image.sun").string(),
                                         dir / "x64.moon", options);
  options.targetTriple = "aarch64-linux-gnu";
  sun::moon_bundling::MoonBuilder::build((dir / "image.sun").string(),
                                         dir / "arm.moon", options);
  auto cfg = config(
      R"({"dependencies":[{"target":{"x86_64-linux-gnu":{"package":"IMAGE_CORE","library":"image_core","path":"x64.moon"},"aarch64-linux-gnu":{"package":"IMAGE_CORE","library":"image_core","path":"arm.moon"}}}]})");
  EXPECT_FALSE(fs::exists(dir / "cache"));
  auto x64 = ManifestProcessor::expandPathVariables(
      "$IMAGE_CORE/lib/image_core.moon", &cfg);
  EXPECT_EQ(sun::driver::packageFileHash(x64),
            sun::driver::packageFileHash(dir / "x64.moon"));
  EXPECT_NE(x64.find(sun::driver::configTargetKey("x86_64-linux-gnu")),
            std::string::npos);
  put(x64, "damaged");
  EXPECT_EQ(ManifestProcessor::expandPathVariables(
                "$IMAGE_CORE/lib/image_core.moon", &cfg),
            x64);
  EXPECT_EQ(sun::driver::packageFileHash(x64),
            sun::driver::packageFileHash(dir / "x64.moon"));
  auto arm = SunConfig::loadFile(dir / "sun-config.json", "aarch64-linux-gnu");
  EXPECT_NE(ManifestProcessor::expandPathVariables(
                "$IMAGE_CORE/lib/image_core.moon", &arm),
            x64);
  // Typed providers select the same verified artifact and reject other targets.
  auto typed = config(R"({"dependencies":[{
    "type":"moon","package":"IMAGE_CORE","library":"image_core",
    "target":{"x86_64-linux-gnu":{"path":"x64.moon"},
              "aarch64-linux-gnu":{"path":"arm.moon"}}
  }]})");
  EXPECT_EQ(ManifestProcessor::expandPathVariables("$IMAGE_CORE/lib/image_core.moon", &typed), x64);
  auto wrongTarget = config(R"({"dependencies":[{
    "type":"moon","package":"IMAGE_CORE","library":"image_core","path":"arm.moon"
  }]})");
  EXPECT_THROW(sun::driver::resolveConfigDependency(wrongTarget, "IMAGE_CORE"), SunError);
  ManifestProcessor::setPathVariable("IMAGE_CORE", "/override");
  EXPECT_THROW(ManifestProcessor::expandPathVariables(
                   "$IMAGE_CORE/lib/image_core.moon", &cfg),
               SunError);
}

/** Unused unavailable targets are allowed, and used ones fail clearly. */
TEST_F(Packages, LazyDependencyTargetAndHash) {
  auto cfg = config(
      R"({"dependencies":[{"target":{"aarch64-linux-gnu":{"package":"LIB","library":"lib","path":"absent.moon"}}}]})");
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "LIB"), SunError);
  EXPECT_NO_THROW(config(
      R"({"dependencies":[{"package":"LIB","library":"lib","url":"https://example.com/lib.moon"}]})"));
  EXPECT_THROW(config(
      R"({"dependencies":[{"package":"LIB","library":"lib","url":"https://example.com/lib.moon","hash":"invalid"}]})"),
      SunError);
  EXPECT_THROW(
      config(
          R"({"dependencies":[{"package":"LIB","library":"../lib","path":"lib.moon"}]})"),
      SunError);
}

/** Multiple outputs and explicit resources survive packaging and cache
 * extraction. */
TEST_F(Packages, RoundTripAndResourceRebuild) {
  put(dir / "app", "binary");
  put(dir / "helpers.moon", "library");
  put(dir / "assets/message.txt", "hello");
  auto cfg = config(R"({"root":true,"entrypoints":[
    {"name":"app","path":"main.sun"},
    {"name":"helpers","path":"helpers.sun","type":"library"},
    {"name":"disabled","enabled":false}
  ],"packages":[{"name":"tools","entrypoints":["app","helpers","disabled"],"resources":[{"path":"assets","destination":"share/app"}]}]})");
  std::vector<sun::driver::PackageArtifact> artifacts{
      {"app", (dir / "app").string(), false},
      {"helpers", (dir / "helpers.moon").string(), true}};
  auto plans = sun::driver::planPackages(cfg, artifacts);
  ASSERT_EQ(plans.size(), 1u);
  EXPECT_EQ(plans[0].output,
            dir / ".sun/build/x86_64-unknown-linux-gnu/release" /
                dir.filename() / "packages/tools");
  cfg.pathVariables["SUN_BUILD"] = (dir / "custom-build").string();
  plans = sun::driver::planPackages(cfg, artifacts);
  EXPECT_EQ(plans[0].output, dir / "custom-build/packages/tools");
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

/** Unpinned URLs follow changed releases while retaining package validation. */
TEST_F(Packages, UnpinnedUrlRefreshesAndRejectsInvalidReleases) {
  put(dir / "app", "first release");
  auto producer = config(R"({"root":true,
    "entrypoints":[{"name":"app","path":"main.sun"}],
    "packages":[{"name":"tools","entrypoints":["app"]}]})");
  auto plans = sun::driver::planPackages(
      producer, {{"app", (dir / "app").string(), false}});
  sun::driver::buildPackages(producer, plans);
  const auto archive = fs::path(plans[0].output.string() + ".tar.gz");
  const auto originalHash = sun::driver::packageFileHash(archive);
  const auto url = "file://" + fs::absolute(archive).string();
  auto consumer = config("{\"dependencies\":[{\"package\":\"TOOLS\",\"url\":\"" +
                         url + "\"}]}");
  const auto first = sun::driver::resolveConfigDependency(consumer, "TOOLS");
  put(dir / "app", "updated release");
  sun::driver::buildPackages(producer, plans);
  const auto updated = sun::driver::resolveConfigDependency(consumer, "TOOLS");
  EXPECT_NE(first, updated);
  EXPECT_EQ(sun::driver::packageFileHash(updated / "bin/app"),
            sun::driver::packageFileHash(dir / "app"));

  auto pinned = config("{\"dependencies\":[{\"package\":\"TOOLS\",\"url\":\"" +
                       url + "\",\"hash\":\"" + originalHash + "\"}]}");
  EXPECT_THROW(sun::driver::resolveConfigDependency(pinned, "TOOLS"), SunError);
  consumer.targetTriple = "aarch64-linux-gnu";
  EXPECT_THROW(sun::driver::resolveConfigDependency(consumer, "TOOLS"), SunError);
  consumer.targetTriple = "x86_64-linux-gnu";
  put(archive, "not a package");
  EXPECT_THROW(sun::driver::resolveConfigDependency(consumer, "TOOLS"), SunError);
  fs::remove(archive);
  EXPECT_THROW(sun::driver::resolveConfigDependency(consumer, "TOOLS"), SunError);
  EXPECT_TRUE(fs::exists(updated / "bin/app"));
}

/** Explicit single-entrypoint packages detect unsafe resources and conflicts.
 */
TEST_F(Packages, SingleEntrypointAndConflicts) {
  put(dir / "asset", "data");
  put(dir / "app", "binary");
  auto cfg = config(
      R"({"entrypoints":[{"name":"app","path":"main.sun"}],"packages":[{"name":"app","entrypoints":["app"],"resources":[{"path":"asset","destination":"share/data"}]}]})");
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
  cfg.packages[0].resources[0].path = (dir / "link").string();
  EXPECT_THROW(sun::driver::planPackages(cfg, artifacts), SunError);
}

/** Entrypoints can export resources alongside their compiled artifact. */
TEST_F(Packages, AcceptEntrypointResources) {
  auto cfg = config(
      R"({"entrypoints":[{"name":"app","path":"main.sun","resources":[{"path":"asset","destination":"share/asset"}]}]})");
  ASSERT_EQ(cfg.entrypoints.size(), 1u);
  ASSERT_EQ(cfg.entrypoints.front().resources.size(), 1u);
  EXPECT_EQ(cfg.entrypoints.front().resources.front().destination,
            "share/asset");
}

/** Packages include resources from selected entrypoints and shared exports. */
TEST_F(Packages, InheritsRuntimeResources) {
  put(dir / "app", "binary");
  put(dir / "asset", "resource");
  put(dir / "shared", "shared resource");
  auto cfg = config(R"({"entrypoints":[{"name":"app","path":"main.sun",
    "resources":[{"path":"asset","destination":"app.asset"}]},
    {"name":"unused","path":"unused.sun","resources":[{"path":"missing","destination":"unused.asset"}]}],
    "exports":[{"path":"shared","destination":"share/shared"}],
    "packages":[{"name":"tools","entrypoints":["app"]}]})");
  cfg.pathVariables["SUN_BUILD"] = (dir / "output").string();
  auto plans =
      sun::driver::planPackages(cfg, {{"app", (dir / "app").string(), false}});
  sun::driver::buildPackages(cfg, plans);
  EXPECT_TRUE(fs::is_regular_file(plans[0].output / "bin/app.asset"));
  EXPECT_TRUE(fs::is_regular_file(plans[0].output / "share/shared"));

}

/** Generated library resources already beside their product need no build copy.
 */
TEST_F(Packages, LibraryResourcesAlreadyInBuildDirectory) {
  put(dir / "output/native/helper.a", "native library");
  put(dir / "output/helpers.moon", "library");
  auto cfg = config(
      R"({"entrypoints":[{"name":"helpers","path":"helpers.sun","type":"library",
    "resources":[{"path":"${SUN_BUILD}/native/helper.a","destination":"native/helper.a"}]}],
    "packages":[{"name":"tools","entrypoints":["helpers"]}]})");
  cfg.pathVariables["SUN_BUILD"] = (dir / "output").string();
  auto plans = sun::driver::planPackages(
      cfg, {{"helpers", (dir / "output/helpers.moon").string(), true}});
  const auto time = fs::last_write_time(dir / "output/native/helper.a");
  sun::driver::buildPackages(cfg, plans);
  EXPECT_EQ(fs::last_write_time(dir / "output/native/helper.a"), time);
  EXPECT_TRUE(fs::is_regular_file(plans[0].output / "lib/native/helper.a"));
}

/** Build copies cannot overwrite products or follow output symlinks. */
TEST_F(Packages, RejectsUnsafeBuildResources) {
  put(dir / "app", "binary");
  put(dir / "asset", "resource");
  auto cfg = config(R"({"entrypoints":[{"name":"app","path":"main.sun",
    "resources":[{"path":"asset","destination":"asset"}]}],
    "packages":[{"name":"tools","entrypoints":["app"]}]})");
  cfg.pathVariables["SUN_BUILD"] = (dir / "output").string();
  auto plans =
      sun::driver::planPackages(cfg, {{"app", (dir / "app").string(), false}});
  for (const auto& destination :
       {"app", "app_test", ".contexts/data"}) {
    cfg.entrypoints[0].resources[0].destination = destination;
    EXPECT_THROW(sun::driver::buildPackages(cfg, plans), SunError)
        << destination;
  }
  fs::create_directories(dir / "output");
  fs::create_directory_symlink(dir, dir / "output/link");
  cfg.entrypoints[0].resources[0].destination = "link/copied";
  EXPECT_THROW(sun::driver::buildPackages(cfg, plans), SunError);
  EXPECT_FALSE(fs::exists(dir / "copied"));
}

/** Naming an entrypoint alone does not request a distribution. */
TEST_F(Packages, NoImplicitPackages) {
  auto cfg = config(R"({"entrypoints":[{"name":"app","path":"main.sun"}]})");
  std::vector<sun::driver::PackageArtifact> artifacts{
      {"app", (dir / "app").string(), false}};
  EXPECT_TRUE(sun::driver::planPackages(cfg, artifacts).empty());
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
  if (link) {
    archive_entry_set_symlink(entry, "/etc/passwd");
  }
  archive_write_header(writer, entry);
  archive_entry_free(entry);
  archive_write_close(writer);
  archive_write_free(writer);
}

/** Extraction never follows links or writes paths outside its private staging
 * root. */
TEST_F(Packages, UnsafeArchives) {
  auto cfg =
      config(R"({"dependencies":[{"package":"BAD","path":"bad.tar.gz"}]})");
  maliciousArchive(dir / "bad.tar.gz", "../escaped", false);
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "BAD"), SunError);
  maliciousArchive(dir / "bad.tar.gz", "link", true);
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "BAD"), SunError);
  EXPECT_FALSE(fs::exists(dir / "escaped"));
}
/** Default caches follow the selected workspace while explicit overrides win.
 */
TEST_F(Packages, WorkspaceDependencyCache) {
  const auto executable = fs::canonical("build/sun");
  auto cfg = config(
      std::string(
          R"({"dependencies":[{"package":"example/compiler","binary":"example/sun","path":")") +
          executable.string() + R"("}]})",
      llvm::sys::getDefaultTargetTriple());
  unsetenv("SUN_DEPENDENCY_CACHE");
  const auto target = sun::driver::configTargetKey(cfg.targetTriple);
  const auto standalone =
      sun::driver::resolveConfigDependency(cfg, "example/compiler");
  EXPECT_EQ(standalone.parent_path(),
            dir / ".sun/cache/dependencies/example/compiler" / target);
  sun::driver::ConfigGraph graph{dir / "workspace", {}};
  sun::driver::ConfigGraphScope scope(graph);
  const auto workspace =
      sun::driver::resolveConfigDependency(cfg, "example/compiler");
  EXPECT_EQ(workspace.parent_path(),
            graph.root / ".sun/cache/dependencies/example/compiler" / target);
  EXPECT_TRUE(fs::is_regular_file(workspace / "bin/sun"));
  setenv("SUN_DEPENDENCY_CACHE", (dir / "override").c_str(), 1);
  const auto overridden =
      sun::driver::resolveConfigDependency(cfg, "example/compiler");
  EXPECT_EQ(overridden.parent_path(),
            dir / "override/example/compiler" / target);
}

/** Individual executables retain executable permissions and reject a foreign
 * target. */
TEST_F(Packages, InstallsSelectedBinaryForItsPlatform) {
  const auto executable = fs::canonical("build/sun");
  auto cfg = config(
      std::string(
          R"({"dependencies":[{"package":"example/compiler","binary":"example/sun","path":")") +
          executable.string() + R"("}]})",
      llvm::sys::getDefaultTargetTriple());
  auto root = sun::driver::resolveConfigDependency(cfg, "example/compiler");
  EXPECT_EQ(sun::driver::packageFileHash(root / "bin/sun"),
            sun::driver::packageFileHash(executable));
  EXPECT_NE(fs::status(root / "bin/sun").permissions() & fs::perms::owner_exec,
            fs::perms::none);
  cfg.targetTriple = sun::driver::configTargetKey(cfg.targetTriple) ==
                             sun::driver::configTargetKey("x86_64-linux-gnu")
                         ? "aarch64-linux-gnu"
                         : "x86_64-linux-gnu";
  EXPECT_THROW(sun::driver::resolveConfigDependency(cfg, "example/compiler"),
               SunError);
}

}  // namespace
