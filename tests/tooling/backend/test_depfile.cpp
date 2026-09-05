// tests/tooling/backend/test_depfile.cpp - The --depfile output
//
// A build system cannot see which files a manifest pulls in, so the
// compiler tells it: one Make-format rule per artifact, naming every input
// that artifact was built from. The rendering is checked directly; the
// end-to-end cases drive the sun binary the way a CMake custom command
// would and read the depfile back.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "driver/depfile.h"

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void writeFile(const std::string& path, const std::string& text) {
  std::ofstream(path) << text;
}

// The rule for `output` in a depfile: from the line naming it up to the
// next unindented line. Empty when the output has no rule.
std::string ruleFor(const std::string& depfile, const std::string& output) {
  const size_t start = depfile.find(output + ":");
  if (start == std::string::npos) return "";
  size_t end = start;
  while (true) {
    end = depfile.find('\n', end);
    if (end == std::string::npos) return depfile.substr(start);
    if (end + 1 >= depfile.size() || depfile[end + 1] != ' ') {
      return depfile.substr(start, end - start);
    }
    ++end;
  }
}

// A scratch folder for one end-to-end case, removed afterwards.
struct Scratch {
  std::filesystem::path dir;
  explicit Scratch(const std::string& name)
      : dir(std::filesystem::path(::testing::TempDir()) /
            ("sun_depfile_" + name + "_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~Scratch() { std::filesystem::remove_all(dir); }
  std::string path(const std::string& name) const {
    return (dir / name).string();
  }
};

bool haveSunBinary() { return std::filesystem::exists("build/sun"); }

}  // namespace

// ============================================================================
// Rendering
// ============================================================================

TEST(Tooling_Backend_Depfile, one_rule_per_output_with_absolute_paths) {
  sun::Depfile depfile;
  depfile.addOutput("/out/lib.moon", {"/src/lib.sun", "/src/util.sun"});
  depfile.addOutput("/out/lib_test", {"/src/lib.sun", "/src/lib_tests.sun"});
  const std::string text = depfile.render();
  EXPECT_EQ(text,
            "/out/lib.moon: \\\n  /src/lib.sun \\\n  /src/util.sun\n"
            "/out/lib_test: \\\n  /src/lib.sun \\\n  /src/lib_tests.sun\n");
}

TEST(Tooling_Backend_Depfile, duplicate_inputs_are_dropped) {
  sun::Depfile depfile;
  depfile.addOutput("/out/a", {"/src/x.sun", "/src/x.sun", "", "/src/y.sun"});
  EXPECT_EQ(depfile.render(), "/out/a: \\\n  /src/x.sun \\\n  /src/y.sun\n");
}

TEST(Tooling_Backend_Depfile, shared_input_reaches_every_rule) {
  sun::Depfile depfile;
  depfile.addOutput("/out/a", {"/src/a.sun"});
  depfile.addOutput("/out/b", {"/src/b.sun"});
  depfile.addSharedInput("/src/sun-config.json");
  const std::string text = depfile.render();
  EXPECT_NE(ruleFor(text, "/out/a").find("/src/sun-config.json"),
            std::string::npos);
  EXPECT_NE(ruleFor(text, "/out/b").find("/src/sun-config.json"),
            std::string::npos);
}

// One `sun -c sun-config.json` builds every entrypoint, so a bundle that
// imports a sibling bundle from the same run must not list it: the command
// would depend on its own output, which Ninja reports as a cycle.
TEST(Tooling_Backend_Depfile, outputs_of_the_same_run_are_not_inputs) {
  sun::Depfile depfile;
  depfile.addOutput("/out/stdlib.moon", {"/src/stdlib.sun"});
  depfile.addOutput("/out/tls.moon",
                    {"/src/tls.sun", "/out/stdlib.moon", "/vendor/libssl.a"});
  EXPECT_EQ(depfile.render(),
            "/out/stdlib.moon: \\\n  /src/stdlib.sun\n"
            "/out/tls.moon: \\\n  /src/tls.sun \\\n  /vendor/libssl.a\n");
}

// Make and Ninja read a backslash before a space or '#', and '$$' for '$'.
TEST(Tooling_Backend_Depfile, special_characters_are_escaped) {
  sun::Depfile depfile;
  depfile.addOutput("/out/my lib.moon", {"/src/a#1.sun", "/src/$x.sun"});
  EXPECT_EQ(depfile.render(),
            "/out/my\\ lib.moon: \\\n  /src/a\\#1.sun \\\n  /src/$$x.sun\n");
}

// ============================================================================
// End to end, through the sun binary
// ============================================================================

// --emit-moon: the bundle depends on the entrypoint and every source_files
// entry, and on nothing else.
TEST(Tooling_Backend_Depfile, emit_moon_lists_the_manifest_sources) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  Scratch scratch("moon");
  writeFile(scratch.path("util.sun"), R"(
public module depfile_lib {
  public function two() i32 { return 2; }
}
)");
  writeFile(scratch.path("lib.sun"), R"(
public module depfile_lib {
  public function one() i32 { return 1; }
}
manifest { source_files: ["util.sun"] }
)");
  const std::string moon = scratch.path("lib.moon");
  const std::string depfile = scratch.path("lib.moon.d");
  const std::string cmd = "build/sun --emit-moon --depfile " + depfile +
                          " -o " + moon + " " + scratch.path("lib.sun") +
                          " > " + scratch.path("log") + " 2>&1";
  int rc = std::system(cmd.c_str());
  ASSERT_EQ(WEXITSTATUS(rc), 0) << readFile(scratch.path("log"));

  const std::string text = readFile(depfile);
  const std::string rule = ruleFor(text, moon);
  EXPECT_FALSE(rule.empty()) << text;
  EXPECT_NE(rule.find(scratch.path("lib.sun")), std::string::npos) << text;
  EXPECT_NE(rule.find(scratch.path("util.sun")), std::string::npos) << text;
}

// -c on a program with tests: the executable's rule leaves the test file
// out, the test binary's rule includes it, and both name the bundle the
// manifest imports.
TEST(Tooling_Backend_Depfile, compile_separates_program_and_test_inputs) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  if (!std::filesystem::exists("build/stdlib.moon")) {
    GTEST_SKIP() << "build/stdlib.moon not found";
  }
  Scratch scratch("compile");
  writeFile(scratch.path("app_tests.sun"), R"(
using std;
module depfile_app {
  test_function answer_is_forty_two() {
    std.test.assert_eq(depfile_app.answer(), 42);
  }
}
)");
  writeFile(scratch.path("app.sun"), R"(
using std;
module depfile_app {
  function answer() i32 { return 42; }
}
function main() i32 { return 0; }
manifest {
  test_files: ["app_tests.sun"]
  libraries: ["stdlib.moon"]
}
)");
  const std::string app = scratch.path("app");
  const std::string depfile = scratch.path("app.d");
  const std::string cmd = "build/sun -c --lib-path build --depfile " + depfile +
                          " -o " + app + " " + scratch.path("app.sun") + " > " +
                          scratch.path("log") + " 2>&1";
  int rc = std::system(cmd.c_str());
  ASSERT_EQ(WEXITSTATUS(rc), 0) << readFile(scratch.path("log"));

  const std::string text = readFile(depfile);
  const std::string appRule = ruleFor(text, app);
  const std::string testRule = ruleFor(text, app + "_test");
  ASSERT_FALSE(appRule.empty()) << text;
  ASSERT_FALSE(testRule.empty()) << text;

  EXPECT_NE(appRule.find(scratch.path("app.sun")), std::string::npos) << text;
  EXPECT_EQ(appRule.find("app_tests.sun"), std::string::npos) << text;
  EXPECT_NE(appRule.find("stdlib.moon"), std::string::npos) << text;

  EXPECT_NE(testRule.find(scratch.path("app.sun")), std::string::npos) << text;
  EXPECT_NE(testRule.find(scratch.path("app_tests.sun")), std::string::npos)
      << text;
  EXPECT_NE(testRule.find("stdlib.moon"), std::string::npos) << text;
}

// The flag describes built artifacts, so it has nothing to say about a JIT
// run.
TEST(Tooling_Backend_Depfile, rejected_without_a_build_mode) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  Scratch scratch("jit");
  writeFile(scratch.path("run.sun"), "function main() i32 { return 0; }\n");
  const std::string cmd = "build/sun --depfile " + scratch.path("run.d") + " " +
                          scratch.path("run.sun") + " > " +
                          scratch.path("log") + " 2>&1";
  int rc = std::system(cmd.c_str());
  EXPECT_NE(WEXITSTATUS(rc), 0);
  EXPECT_NE(readFile(scratch.path("log")).find("--depfile"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(scratch.path("run.d")));
}
