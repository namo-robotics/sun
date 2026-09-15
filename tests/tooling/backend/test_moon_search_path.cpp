#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string quote(const fs::path& path) {
  std::string result = "'";
  for (char c : path.string()) {
    result += c == '\'' ? "'\\''" : std::string(1, c);
  }
  return result + "'";
}

std::string readFile(const fs::path& path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// A relocated installation keeps these tests independent of system packages.
class MoonSearchPath : public testing::TestWithParam<const char*> {
 protected:
  fs::path dir;
  fs::path dependencyPath;

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
    {"path": "lib.sun", "type": "library", "output_name": "library"}
  ]
})";
  }

  void TearDown() override { fs::remove_all(dir); }

  int run(const std::string& command) {
    return std::system(
        ("env SUN_PATH= " + command + " > " + quote(dir / "log") + " 2>&1")
            .c_str());
  }

  void checkBuild(const std::string& arguments) {
    ASSERT_EQ(run(quote(dir / "bin/sun") + " --depfile " +
                  quote(dir / "output.d") + " " + arguments),
              0)
        << readFile(dir / "log");
    EXPECT_TRUE(fs::exists(dir / "project/library.moon"));
    EXPECT_NE(readFile(dir / "output.d").find(dependencyPath.string()),
              std::string::npos);
  }
};

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
  for (const char* target : {"aarch64-linux-gnu", "aarch64-linux-musl",
                             "aarch64-unknown-linux-gnu"}) {
    const std::string flags = std::string("--target ") + target + " ";
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
          "output_name": "cross/library"
        }
      ]
    }
  },
  "entrypoints": [
    {
      "path": "lib.sun",
      "type": "library",
      "output_name": "native/library"
    }
  ]
})";
  ASSERT_EQ(run(quote(dir / "bin/sun") +
                " -c --target aarch64-unknown-linux-gnu --no-test --depfile " +
                quote(dir / "output.d") + " " +
                quote(dir / "project/sun-config.json")),
            0)
      << readFile(dir / "log");
  EXPECT_TRUE(fs::exists(dir / "project/cross/library.moon"));
  EXPECT_FALSE(fs::exists(dir / "project/native/library.moon"));
  EXPECT_NE(readFile(dir / "output.d").find(dependencyPath.string()),
            std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(Tooling_Backend, MoonSearchPath,
                         testing::Values("lib/sun", "share/sun/stdlib"));

}  // namespace
