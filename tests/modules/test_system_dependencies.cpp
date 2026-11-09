// Exercise dependency installation through isolated package-manager substitutes.
#include <gtest/gtest.h>
#include <llvm/TargetParser/Host.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "cli/config_build_command.h"
#include "driver/system_dependencies.h"
#include "driver/package.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Keeps process-environment fixtures private to these tests. */
namespace {
/** Owns fake host tools so tests never install packages or access the network. */
class SystemDependencies : public testing::Test {
 protected:
  std::filesystem::path dir;
  std::string oldPath;
  sun::driver::SunConfig config;

  /** Writes an executable fake tool or plain test input. */
  void write(const std::string& name, const std::string& contents, bool executable = false) {
    std::filesystem::create_directories((dir / name).parent_path());
    std::ofstream(dir / name) << contents;
    if (executable) std::filesystem::permissions(dir / name, std::filesystem::perms::owner_all);
  }
  /** Sets up a host-target config and mock package-manager commands. */
  void SetUp() override {
    dir = std::filesystem::current_path() / "tmp/system-dependency-tests" /
        testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    setenv("PATH", (dir.string() + ":" + oldPath).c_str(), 1);
    config.configDir = dir;
    config.configFile = dir / "sun-config.json";
    config.targetTriple = llvm::sys::getDefaultTargetTriple();
    config.packageId = "example/app";
    write("pkg-config", "#!/bin/sh\n[ -f '" + (dir / "installed").string() + "' ]\n", true);
    write("apt-get", "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + (dir / "args").string() +
        "'\n: > '" + (dir / "installed").string() + "'\n", true);
    write("brew", "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + (dir / "args").string() +
        "'\n: > '" + (dir / "installed").string() + "'\n", true);
    write("sudo", "#!/bin/sh\nexec \"$@\"\n", true);
    sun::driver::ConfigSystemDependency dep;
    dep.type = "system";
    dep.check = sun::driver::ConfigDependencyCheck{
        sun::driver::ConfigDependencyCheck::Type::PkgConfig, "example-lib", ">=3.2"};
    dep.apt = {"libexample-dev"};
    dep.brew = {"example"};
    config.systemDependencies.push_back(dep);
  }
  /** Restores PATH and removes fixture files. */
  void TearDown() override {
    setenv("PATH", oldPath.c_str(), 1);
    std::filesystem::remove_all(dir);
  }
};
}

/** Ordinary checks aggregate missing modules without invoking the installer. */
TEST_F(SystemDependencies, ChecksNeverInstall) {
  config.systemDependencies.push_back(config.systemDependencies.front());
  config.systemDependencies.back().check->name = "another-lib";
  try {
    sun::driver::checkSystemDependencies(config);
    FAIL() << "missing dependencies should fail";
  } catch (const sun::support::SunError& error) {
    EXPECT_NE(std::string(error.what()).find("example-lib"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("another-lib"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
  EXPECT_FALSE(std::filesystem::exists(dir / "tmp"));
}

/** Previewing installation never invokes a package manager. */
TEST_F(SystemDependencies, DryRunDoesNotInstall) {
  sun::driver::installSystemDependencies(config, true, false);
  EXPECT_FALSE(std::filesystem::exists(dir / "installed"));
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
}

/** Explicit installation satisfies checks and leaves acceptance prompts enabled. */
TEST_F(SystemDependencies, InstallsAndReusesSystemPackage) {
  sun::driver::installSystemDependencies(config, false, false);
  EXPECT_NO_THROW(sun::driver::checkSystemDependencies(config));
  std::ifstream input(dir / "args");
  std::string args((std::istreambuf_iterator<char>(input)), {});
  EXPECT_EQ(args.find("-y"), std::string::npos);
  std::filesystem::remove(dir / "args");
  sun::driver::installSystemDependencies(config, false, true);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
}

/** A foreign target must never cause installation into the host filesystem. */
TEST_F(SystemDependencies, RejectsCrossTargetInstallation) {
  config.targetTriple = "aarch64-linux-musl";
  EXPECT_THROW(sun::driver::installSystemDependencies(config, false, true), sun::support::SunError);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
}

/** Debian downloads are not fetched when previewing an installation plan. */
TEST_F(SystemDependencies, DebianDryRunDoesNotDownload) {
#ifdef __linux__
  config.systemDependencies.clear();
  sun::driver::ConfigSystemDependency dep;
  dep.type = "deb";
  dep.url = "https://invalid.example/lib.deb";
  dep.hash = std::string(64, 'a');
  config.systemDependencies.push_back(dep);
  EXPECT_NO_THROW(sun::driver::installSystemDependencies(config, true, false));
  EXPECT_FALSE(std::filesystem::exists(dir / ".sun"));
#endif
}

/** The CLI selects a config and previews its typed requirements without side effects. */
TEST_F(SystemDependencies, InstallCommandDryRun) {
  write("sun-config.json", R"({"dependencies":[{
    "type":"system","check":{"type":"pkg-config","name":"example-lib"},"apt":["libexample-dev"],"brew":["example"]
  }]})");
  EXPECT_EQ(sun::cli::runDependencyCommand({"install", "--config", config.configFile.string(), "--dry-run"}), 0);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
}

/** A bad pinned Debian digest fails before inspecting or installing the package. */
TEST_F(SystemDependencies, RejectsDebianChecksumBeforeInstallation) {
#ifdef __linux__
  write("package.deb", "not the pinned contents");
  sun::driver::ConfigSystemDependency dep;
  dep.type = "deb";
  dep.path = (dir / "package.deb").string();
  dep.hash = std::string(64, 'a');
  config.systemDependencies = {dep};
  EXPECT_THROW(sun::driver::installSystemDependencies(config, false, false), sun::support::SunError);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
#endif
}

/** Debian installation checks architecture and reuses the exact installed version. */
TEST_F(SystemDependencies, DebianInstallAndSubsequentCheck) {
#ifdef __linux__
  write("package.deb", "test archive inspected by mock dpkg-deb");
  write("dpkg-deb", "#!/bin/sh\ncase \"$3\" in\nPackage) echo example-dev;;\nVersion) echo 1.2;;\nArchitecture) echo amd64;;\nesac\n", true);
  write("dpkg", "#!/bin/sh\necho amd64\n", true);
  write("dpkg-query", "#!/bin/sh\n[ -f '" + (dir / "installed").string() + "' ] || exit 1\nprintf 'install ok installed\\n1.2'\n", true);
  sun::driver::ConfigSystemDependency dep;
  dep.type = "deb";
  dep.path = (dir / "package.deb").string();
  dep.hash = sun::driver::packageFileHash(dep.path);
  config.systemDependencies = {dep};
  EXPECT_THROW(sun::driver::checkSystemDependencies(config), sun::support::SunError);
  sun::driver::installSystemDependencies(config, false, true);
  EXPECT_NO_THROW(sun::driver::checkSystemDependencies(config));
  // The verified cache remains usable after the original download is removed.
  std::filesystem::remove(dir / "package.deb");
  EXPECT_NO_THROW(sun::driver::checkSystemDependencies(config));
  std::filesystem::remove(dir / "args");
  sun::driver::installSystemDependencies(config, false, true);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
  std::filesystem::remove(dir / "installed");
  EXPECT_THROW(sun::driver::checkSystemDependencies(config), sun::support::SunError);
  write("dpkg", "#!/bin/sh\necho arm64\n", true);
  EXPECT_THROW(sun::driver::installSystemDependencies(config, false, true), sun::support::SunError);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
#endif
}

/** Missing system requirements stop a build before any native step is executed. */
TEST_F(SystemDependencies, BuildChecksBeforeNativeSteps) {
  write("step.sh", "#!/bin/sh\n: > '" + (dir / "ran").string() + "'\n", true);
  write("sun-config.json", R"({
    "package_id":"example/app",
    "dependencies":[{"type":"system","check":{"type":"pkg-config","name":"example-lib"},"apt":["libexample-dev"]}],
    "entrypoints":[{"name":"app","path":"main.sun","steps":["native"],"export":true}],
    "steps":[{"name":"native","command":["sh","step.sh"],"inputs":["step.sh"],"outputs":["${SUN_BUILD}/done"]}]
  })");
  testing::internal::CaptureStderr();
  auto result = sun::cli::runBuildCommand({"--config", config.configFile.string()});
  const auto diagnostics = testing::internal::GetCapturedStderr();
  EXPECT_NE(result, 0);
  EXPECT_NE(diagnostics.find("missing system dependencies"), std::string::npos) << diagnostics;
  EXPECT_FALSE(std::filesystem::exists(dir / "ran"));
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
}

/** Unchecked packages skip build checks and delegate installation to the manager. */
TEST_F(SystemDependencies, NoCheckDelegatesToPackageManager) {
  config.systemDependencies.front().check.reset();
  EXPECT_NO_THROW(sun::driver::checkSystemDependencies(config));
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
  sun::driver::installSystemDependencies(config, true, false);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
  // A package-manager success must not require pkg-config to become available.
  write("apt-get", "#!/bin/sh\n: > '" + (dir / "args").string() + "'\n", true);
  write("brew", "#!/bin/sh\n: > '" + (dir / "args").string() + "'\n", true);
  EXPECT_NO_THROW(sun::driver::installSystemDependencies(config, false, false));
  EXPECT_TRUE(std::filesystem::exists(dir / "args"));
  EXPECT_FALSE(std::filesystem::exists(dir / "installed"));
}

/** Executable checks inspect PATH without executing the program or pkg-config. */
TEST_F(SystemDependencies, ExecutableChecksOnlySearchPath) {
  config.systemDependencies.front().check = sun::driver::ConfigDependencyCheck{
      sun::driver::ConfigDependencyCheck::Type::Executable, "sun-fixture-tool", ""};
  EXPECT_THROW(sun::driver::checkSystemDependencies(config), sun::support::SunError);
  write("sun-fixture-tool", "#!/bin/sh\nexit 99\n", true);
  EXPECT_NO_THROW(sun::driver::checkSystemDependencies(config));
  sun::driver::installSystemDependencies(config, false, false);
  EXPECT_FALSE(std::filesystem::exists(dir / "args"));
  // Host build tools are discoverable even when the output target differs.
  config.targetTriple = "aarch64-linux-musl";
  EXPECT_NO_THROW(sun::driver::checkSystemDependencies(config));
}

/** A successful installer must still satisfy the requested executable check. */
TEST_F(SystemDependencies, ExecutableCheckVerifiedAfterInstall) {
  config.systemDependencies.front().check = sun::driver::ConfigDependencyCheck{
      sun::driver::ConfigDependencyCheck::Type::Executable, "sun-fixture-tool", ""};
  EXPECT_THROW(sun::driver::installSystemDependencies(config, false, false), sun::support::SunError);
  const auto script = "#!/bin/sh\nprintf '#!/bin/sh\\nexit 0\\n' > '" +
      (dir / "sun-fixture-tool").string() + "'\nchmod +x '" +
      (dir / "sun-fixture-tool").string() + "'\n";
  write("apt-get", script, true);
  write("brew", script, true);
  EXPECT_NO_THROW(sun::driver::installSystemDependencies(config, false, false));
  EXPECT_NO_THROW(sun::driver::checkSystemDependencies(config));
}

/** Local providers supply requirements without compiling or running their scripts. */
TEST_F(SystemDependencies, InstallLocalDependenciesWithoutBuilding) {
  write("sun-config.json", R"({"root":true,"package_id":"example/app",
    "projects":["provider/sun-config.json"],
    "dependencies":[{"package":"example/provider","entrypoints":["library"]}]
  })");
  write("provider/sun-config.json", R"({"root":true,"package_id":"example/provider",
    "entrypoints":[{"name":"library","path":"missing.sun","type":"library",
      "build":["missing-build-script.sh"]}],
    "dependencies":[{"type":"system","apt":["libexample-dev"],"brew":["example"]}]
  })");
  EXPECT_EQ(sun::cli::runDependencyCommand({"install", "--config", config.configFile.string(),
                                          "--package", "example/app"}), 0);
  EXPECT_TRUE(std::filesystem::exists(dir / "installed"));
  EXPECT_FALSE(std::filesystem::exists(dir / ".sun/build"));
}

/** Cached Git configs are inspected recursively without invoking Git or build steps. */
TEST_F(SystemDependencies, InstallGitDependenciesWithoutBuilding) {
  const std::string url = "https://invalid.example/provider.git";
  const auto cache = dir / "git-cache";
  const auto repository = cache / sun::moon_bundling::computeSha256Hex(url + "\nmain");
  const auto snapshot = repository / "snapshot";
  std::filesystem::create_directories(snapshot / ".sun-git-source");
  std::ofstream(repository / "current") << "snapshot\n";
  write(snapshot.lexically_relative(dir).string() + "/sun-config.json", R"({
    "root":true,"package_id":"example/provider",
    "entrypoints":[{"name":"library","path":"missing.sun","type":"library",
      "build":["missing-build-script.sh"]}],
    "dependencies":[{"type":"system","apt":["libexample-dev"],"brew":["example"]}]
  })");
  write("sun-config.json", R"({"root":true,"dependencies":[{
    "type":"git","url":"https://invalid.example/provider.git","version":"main",
    "packages":{"example/provider":{"entrypoints":["library"]}}
  }]})");
  write("git", "#!/bin/sh\nexit 99\n", true);
  const char* previous = std::getenv("SUN_GIT_CACHE");
  const std::optional<std::string> oldCache = previous ? std::optional<std::string>(previous) : std::nullopt;
  setenv("SUN_GIT_CACHE", cache.c_str(), 1);
  EXPECT_EQ(sun::cli::runDependencyCommand({"install", "--config", config.configFile.string()}), 0);
  if (oldCache) setenv("SUN_GIT_CACHE", oldCache->c_str(), 1);
  else unsetenv("SUN_GIT_CACHE");
  EXPECT_TRUE(std::filesystem::exists(dir / "installed"));
  EXPECT_FALSE(std::filesystem::exists(snapshot / ".sun/build"));
}

/** Installing prebuilt binaries stages their bytes without invoking the compiler. */
TEST_F(SystemDependencies, InstallPrebuiltDependency) {
  const auto binary = std::filesystem::canonical("build/sun");
  write("sun-config.json", std::string(R"({"root":true,"dependencies":[{
    "type":"binary","package":"example/tools","binary":"example/sun","path":")") +
      binary.string() + R"("}]})");
  EXPECT_EQ(sun::cli::runDependencyCommand({"install", "--config", config.configFile.string()}), 0);
  bool found = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir / ".sun/cache/dependencies")) {
    if (entry.is_regular_file() && entry.path().filename() == "sun") {
      EXPECT_EQ(sun::driver::packageFileHash(entry.path()), sun::driver::packageFileHash(binary));
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

/** A dependency preview never fetches remote sources or prebuilt artifacts. */
TEST_F(SystemDependencies, DryRunDoesNotFetchDependencies) {
  write("sun-config.json", R"({"root":true,"dependencies":[
    {"type":"git","url":"https://invalid.example/provider.git","version":"main",
     "packages":{"example/provider":{"entrypoints":["library"]}}},
    {"type":"moon","package":"example/moon","url":"https://invalid.example/lib.moon",
     "sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
  ]})");
  const auto marker = (dir / "fetched").string();
  write("git", "#!/bin/sh\n: > '" + marker + "'\nexit 99\n", true);
  write("curl", "#!/bin/sh\n: > '" + marker + "'\nexit 99\n", true);
  EXPECT_EQ(sun::cli::runDependencyCommand({"install", "--config", config.configFile.string(),
                                          "--dry-run"}), 0);
  EXPECT_FALSE(std::filesystem::exists(dir / "fetched"));
  EXPECT_FALSE(std::filesystem::exists(dir / ".sun"));
}

/** Invalid product selections fail before installing a provider's system packages. */
TEST_F(SystemDependencies, RejectsUnknownDependencyProduct) {
  write("sun-config.json", R"({"root":true,"package_id":"example/app",
    "projects":["provider/sun-config.json"],
    "dependencies":[{"package":"example/provider","entrypoints":["typo"]}]
  })");
  write("provider/sun-config.json", R"({"root":true,"package_id":"example/provider",
    "entrypoints":[{"name":"library","path":"missing.sun","type":"library"}],
    "dependencies":[{"type":"system","apt":["libexample-dev"],"brew":["example"]}]
  })");
  EXPECT_NE(sun::cli::runDependencyCommand({"install", "--config", config.configFile.string(),
                                          "--package", "example/app"}), 0);
  EXPECT_FALSE(std::filesystem::exists(dir / "installed"));
}

/** A provider reached both as a dependency and a project is installed only once. */
TEST_F(SystemDependencies, InstallsSharedProviderOnce) {
  write("sun-config.json", R"({"root":true,"package_id":"example/app",
    "projects":["provider/sun-config.json"],
    "dependencies":[{"package":"example/provider","entrypoints":["library"]}]
  })");
  write("provider/sun-config.json", R"({"root":true,"package_id":"example/provider",
    "entrypoints":[{"name":"library","path":"missing.sun","type":"library"}],
    "dependencies":[{"type":"system","apt":["libexample-dev"],"brew":["example"]}]
  })");
  const auto script = "#!/bin/sh\necho installed >> '" + (dir / "calls").string() + "'\n";
  write("apt-get", script, true);
  write("brew", script, true);
  EXPECT_EQ(sun::cli::runDependencyCommand({"install", "--config", config.configFile.string()}), 0);
  std::ifstream input(dir / "calls");
  const std::string calls((std::istreambuf_iterator<char>(input)), {});
  EXPECT_EQ(calls, "installed\n");
}
