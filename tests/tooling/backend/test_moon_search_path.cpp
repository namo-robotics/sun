#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "driver/build_graph.h"
#include "moon_bundling/moon.h"

/** Keeps test fixtures and helpers local to this source file. */
namespace {

namespace fs = std::filesystem;

/** Quotes a shell argument without changing its literal contents. */
std::string quote(const fs::path& path) {
  std::string result = "'";
  for (char c : path.string()) {
    result += c == '\'' ? "'\\''" : std::string(1, c);
  }
  return result + "'";
}

/** Reads a fixture file into a string for comparison. */
std::string readFile(const fs::path& path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

/**
 * A relocated installation keeps these tests independent of system packages.
 */
class MoonSearchPath : public testing::TestWithParam<const char*> {
 protected:
  fs::path dir;
  fs::path dependencyPath;

  /** Prepares the files and compiler state needed by each test. */
  void SetUp() override {
    ASSERT_TRUE(fs::exists("build/sun"));
    dir = fs::absolute(fs::path("tmp") /
                       ("sun_moon_search_" + std::to_string(getpid())));
    fs::remove_all(dir);
    fs::create_directories(dir / "bin");
    fs::create_directories(dir / "project");
    fs::create_directories(dir / GetParam());
    fs::copy_file("build/sun", dir / "bin/sun");
    dependencyPath = dir / GetParam() / "stdlib.moon";

    // Stop the workspace configuration from supplying its build directory.
    std::ofstream(dir / "sun-config.json") << R"({"root": true})";
    std::ofstream(dir / "dependency.sun") << R"(
// A dependency supplied by the installation.
public module installed_dependency {
  // Return a value for the importing library.
  public function answer() i32 { return 42; }
}
)";
    ASSERT_EQ(run("build/sun --emit-moon -o " + quote(dependencyPath) + " " +
                  quote(dir / "dependency.sun")),
              0)
        << readFile(dir / "log");

    std::ofstream(dir / "project/lib.sun") << R"(
manifest { libraries: ["stdlib.moon"] }
// A library that calls its installed dependency.
public module library {
  // Forward the dependency's result.
  public function answer() i32 { return installed_dependency.answer(); }
}
)";
    std::ofstream(dir / "project/sun-config.json") << R"({
  "root": true,
  "entrypoints": [
    {"path": "lib.sun", "type": "library", "name": "library"}
  ]
})";
  }

  /** Releases the temporary files and state created for each test. */
  void TearDown() override { fs::remove_all(dir); }

  /** Enables import logs for path assertions and executes the fixture program.
   */
  int run(const std::string& command) {
    return std::system(("env SUN_PATH= SUN_LOG_LEVEL=debug " + command + " > " +
                        quote(dir / "log") + " 2>&1")
                           .c_str());
  }

  /** Returns the compiler-owned directory for configured fixture artifacts. */
  fs::path configuredOutput(const std::string& target = "") {
    auto file = dir / "project/sun-config.json";
    auto graph = sun::driver::loadConfigGraph(file, target);
    return sun::driver::configureBuildPaths(
               graph, sun::driver::SunConfig::loadFile(file, target))
        .pathVariables.at("SUN_BUILD");
  }

  /** Builds the fixture and checks that the expected library was selected. */
  void checkBuild(const std::string& arguments) {
    ASSERT_EQ(run(quote(dir / "bin/sun") + " " + arguments), 0)
        << readFile(dir / "log");
    std::string target;
    if (const auto flag = arguments.find("--target ");
        flag != std::string::npos) {
      std::istringstream(arguments.substr(flag + 9)) >> target;
    }
    EXPECT_TRUE(fs::exists(arguments.find("--emit-moon") != std::string::npos
                               ? dir / "project/library.moon"
                               : configuredOutput(target) / "library.moon"));
    // Direct builds display paths relative to the working directory. Config
    // builds keep this dependency absolute because it is outside the project.
    const auto displayedDependency =
        arguments.find("--emit-moon") != std::string::npos
            ? dependencyPath.lexically_relative(fs::current_path())
            : dependencyPath;
    EXPECT_NE(readFile(dir / "log").find("Moon import: " +
                                        displayedDependency.string()),
              std::string::npos)
        << readFile(dir / "log");
  }
};

/** A config build must import the replacement bundle, not its cached
 * predecessor. */
TEST_P(MoonSearchPath, config_rebuild_reloads_replaced_library) {
  const auto config = dir / "project/sun-config.json";
  const auto library = configuredOutput() / "library.moon";
  fs::create_directories(library.parent_path());
  ASSERT_EQ(run(quote(dir / "bin/sun") + " --emit-moon -o " + quote(library) +
                " " + quote(dir / "project/lib.sun")),
            0)
      << readFile(dir / "log");
  std::ofstream(dir / "project/lib.sun") << R"(
manifest { libraries: ["stdlib.moon"] }
/** Exposes a changed implementation through a rebuilt bundle. */
public module library {
  /** Makes the replacement distinguishable from the previous bundle. */
  public function answer() i32 { return installed_dependency.answer() + 1; }
}
)";
  std::ofstream(dir / "project/main.sun") << R"(
manifest { libraries: ["library.moon"] }
/** Verifies the consumer uses the rebuilt implementation. */
function main() i32 { if library.answer() == 43 { return 0; } return 1; }
)";
  std::ofstream(config) << R"({
    "root": true,
    "sun_path": ["."],
    "entrypoints": [
      {"path": "lib.sun", "type": "library", "name": "library"},
      {"path": "main.sun", "type": "binary", "name": "main"}
    ]
  })";
  ASSERT_EQ(run(quote(dir / "bin/sun") + " -c --no-test --force-rebuild " +
                quote(config)),
            0)
      << readFile(dir / "log");
  EXPECT_EQ(run(quote(configuredOutput() / "main")), 0)
      << readFile(dir / "log");
}

/** Direct builds derive identity from the source, independently of output
 * paths. */
TEST_P(MoonSearchPath, direct_identity_defaults_and_override) {
  std::ofstream(dir / "project/sun-config.json")
      << R"({"root":true,"package_id":"example/package"})";
  const auto output = dir / "renamed-output.moon";
  const auto command = quote(dir / "bin/sun") + " --emit-moon -o " +
                       quote(output) + " " + quote(dir / "project/lib.sun");
  ASSERT_EQ(run(command), 0) << readFile(dir / "log");
  auto reader = sun::moon_bundling::MoonReader::open(output);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->getLibrary().bundle_contract().library_id(),
            "example/package/lib");
  ASSERT_EQ(run(command + " --id public_api"), 0) << readFile(dir / "log");
  reader = sun::moon_bundling::MoonReader::open(output);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->getLibrary().bundle_contract().library_id(),
            "example/package/public_api");
  EXPECT_NE(run(command + " --id ''"), 0);
}

/** Ordinary and custom configured builds preserve the declared entrypoint name.
 */
TEST_P(MoonSearchPath, configured_identity_uses_entrypoint_name) {
  const auto configFile = dir / "project/sun-config.json";
  std::ofstream(dir / "project/build.sh")
      << "#!/bin/sh\n# Compile using the configured artifact identity.\n"
         "exec \"$SUN_EXECUTABLE\" build-compile\n";
  fs::permissions(dir / "project/build.sh", fs::perms::owner_exec,
                  fs::perm_options::add);
  for (const bool custom : {false, true}) {
    SCOPED_TRACE(custom ? "custom" : "ordinary");
    std::ofstream(configFile)
        << R"({"root":true,"package_id":"example/package","entrypoints":[{
          "path":"lib.sun","type":"library","name":"public_api","export":true)"
        << (custom ? R"(,"build":["build.sh"])" : "") << "}]}";
    const auto command = quote(dir / "bin/sun") +
                         " build --no-test --force-rebuild --config " +
                         quote(configFile);
    ASSERT_EQ(run(command), 0) << readFile(dir / "log");
    auto reader = sun::moon_bundling::MoonReader::open(configuredOutput() /
                                                       "public_api.moon");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->getLibrary().bundle_contract().library_id(),
              "example/package/public_api");
    EXPECT_NE(run(command + " --id overridden"), 0);
  }
  std::ofstream(dir / "project/build.sh")
      << "#!/bin/sh\n# Attempt a forbidden configured identity override.\n"
         "exec \"$SUN_EXECUTABLE\" build-compile --id overridden\n";
  EXPECT_NE(run(quote(dir / "bin/sun") + " build --no-test --config " +
                quote(configFile)),
            0);
}

TEST_P(MoonSearchPath, emit_moon_finds_installed_dependency) {
  checkBuild("--emit-moon -o " + quote(dir / "project/library.moon") + " " +
             quote(dir / "project/lib.sun"));
}

TEST_P(MoonSearchPath, config_library_finds_installed_dependency) {
  checkBuild("-c " + quote(dir / "project/sun-config.json"));
}

TEST_P(MoonSearchPath, explicit_library_path_precedes_installation) {
  fs::create_directories(dir / "override");
  const auto overrideMoon = dir / "override/stdlib.moon";
  fs::copy_file(dependencyPath, overrideMoon);
  dependencyPath = overrideMoon;
  checkBuild("--lib-path " + quote(dir / "override") + " --emit-moon -o " +
             quote(dir / "project/library.moon") + " " +
             quote(dir / "project/lib.sun"));
}

TEST_P(MoonSearchPath, cross_target_selects_installed_bundle) {
  const auto nativeMoon = dependencyPath;
  dependencyPath = nativeMoon.parent_path() / "aarch64-linux-gnu/stdlib.moon";
  fs::create_directories(dependencyPath.parent_path());
  ASSERT_EQ(run("build/sun --emit-moon --target aarch64-linux-gnu -o " +
                quote(dependencyPath) + " " + quote(dir / "dependency.sun")),
            0)
      << readFile(dir / "log");
  // The spellings name one target, so from the second round on the
  // bundle would be up to date and the build would not name its imports;
  // force it so each spelling is seen to select the installed bundle.
  for (const char* target :
       {"aarch64-linux-gnu", "aarch64-unknown-linux-gnu"}) {
    const std::string flags =
        std::string("--force-rebuild --target ") + target + " ";
    checkBuild(flags + "--emit-moon -o " + quote(dir / "project/library.moon") +
               " " + quote(dir / "project/lib.sun"));
    checkBuild(flags + "-c --no-test " +
               quote(dir / "project/sun-config.json"));
  }
}

TEST_P(MoonSearchPath, command_line_moon_selects_installed_target) {
  dependencyPath =
      dependencyPath.parent_path() / "aarch64-linux-gnu/stdlib.moon";
  fs::create_directories(dependencyPath.parent_path());
  ASSERT_EQ(run("build/sun --emit-moon --target aarch64-linux-gnu -o " +
                quote(dependencyPath) + " " + quote(dir / "dependency.sun")),
            0);
  auto source = readFile(dir / "project/lib.sun");
  source.erase(source.find("manifest"), source.find('}') + 1);
  std::ofstream(dir / "project/lib.sun") << source;
  checkBuild("--target aarch64-linux-gnu --moon stdlib.moon --emit-moon -o " +
             quote(dir / "project/library.moon") + " " +
             quote(dir / "project/lib.sun"));
}

TEST_P(MoonSearchPath, explicit_cross_bundle_precedes_installation) {
  dependencyPath = dir / "override/stdlib.moon";
  fs::create_directories(dependencyPath.parent_path());
  ASSERT_EQ(run("build/sun --emit-moon --target aarch64-linux-gnu -o " +
                quote(dependencyPath) + " " + quote(dir / "dependency.sun")),
            0);
  checkBuild("--target aarch64-linux-gnu --lib-path " +
             quote(dir / "override") + " --emit-moon -o " +
             quote(dir / "project/library.moon") + " " +
             quote(dir / "project/lib.sun"));
}

TEST_P(MoonSearchPath, explicit_wrong_target_is_not_replaced) {
  const auto targetMoon =
      dependencyPath.parent_path() / "aarch64-linux-gnu/stdlib.moon";
  fs::create_directories(targetMoon.parent_path());
  ASSERT_EQ(run("build/sun --emit-moon --target aarch64-linux-gnu -o " +
                quote(targetMoon) + " " + quote(dir / "dependency.sun")),
            0);
  fs::create_directories(dir / "override");
  ASSERT_EQ(run("build/sun --emit-moon --target x86_64-linux-gnu -o " +
                quote(dir / "override/stdlib.moon") + " " +
                quote(dir / "dependency.sun")),
            0);
  EXPECT_NE(
      run(quote(dir / "bin/sun") + " --target aarch64-linux-gnu --lib-path " +
          quote(dir / "override") + " --emit-moon -o " +
          quote(dir / "project/library.moon") + " " +
          quote(dir / "project/lib.sun")),
      0);
  EXPECT_NE(readFile(dir / "log").find("current target"), std::string::npos);
}

TEST_P(MoonSearchPath, explicit_relative_bundle_path_is_literal) {
  const auto nested =
      dependencyPath.parent_path() / "aarch64-linux-gnu/custom/stdlib.moon";
  fs::create_directories(nested.parent_path());
  fs::copy_file(dependencyPath, nested);
  dependencyPath = dependencyPath.parent_path() / "custom/stdlib.moon";
  fs::create_directories(dependencyPath.parent_path());
  ASSERT_EQ(run("build/sun --emit-moon --target aarch64-linux-gnu -o " +
                quote(dependencyPath) + " " + quote(dir / "dependency.sun")),
            0);
  auto source = readFile(dir / "project/lib.sun");
  source.replace(source.find("stdlib.moon"), std::string("stdlib.moon").size(),
                 "custom/stdlib.moon");
  std::ofstream(dir / "project/lib.sun") << source;
  checkBuild("--target aarch64-linux-gnu --emit-moon -o " +
             quote(dir / "project/library.moon") + " " +
             quote(dir / "project/lib.sun"));
}

TEST_P(MoonSearchPath, config_target_selects_outputs_and_dependencies) {
  dependencyPath = dir / "arm/stdlib.moon";
  fs::create_directories(dependencyPath.parent_path());
  ASSERT_EQ(run("build/sun --emit-moon --target aarch64-linux-gnu -o " +
                quote(dependencyPath) + " " + quote(dir / "dependency.sun")),
            0);
  std::ofstream(dir / "project/sun-config.json") << R"({
  "root": true,
  "target": {
    "aarch64-linux-gnu": {
      "sun_path": [
        "../arm"
      ],
      "entrypoints": [
        {
          "path": "lib.sun",
          "type": "library",
          "name": "cross-library"
        }
      ]
    }
  },
  "entrypoints": [
    {
      "path": "lib.sun",
      "type": "library",
      "name": "native-library"
    }
  ]
})";
  ASSERT_EQ(run(quote(dir / "bin/sun") +
                " -c --target aarch64-unknown-linux-gnu --no-test " +
                quote(dir / "project/sun-config.json")),
            0)
      << readFile(dir / "log");
  EXPECT_TRUE(
      fs::exists(configuredOutput("aarch64-linux-gnu") / "cross-library.moon"));
  EXPECT_FALSE(fs::exists(configuredOutput() / "native-library.moon"));
  EXPECT_NE(readFile(dir / "log").find("Moon import: " + dependencyPath.string()),
            std::string::npos)
      << readFile(dir / "log");
}

INSTANTIATE_TEST_SUITE_P(Tooling_Backend, MoonSearchPath,
                         testing::Values("lib/sun", "share/sun/stdlib"));

}  // namespace
