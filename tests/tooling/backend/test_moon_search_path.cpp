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

INSTANTIATE_TEST_SUITE_P(Tooling_Backend, MoonSearchPath,
                         testing::Values("lib/sun", "share/sun/stdlib"));

}  // namespace
