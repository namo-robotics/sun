// tests/tooling/backend/test_input_hash.cpp - Skipping builds by input hash
//
// With --skip-if-unchanged, every artifact records the hash of the inputs it
// was built from, and a run that arrives at the same hash leaves the
// artifact alone. The hash is checked directly; the end-to-end cases drive
// the sun binary the way a build tool running it unconditionally would, and
// look at what it rebuilt.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "driver/build_record.h"
#include "driver/input_hash.h"

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

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// A scratch folder for one end-to-end case, removed afterwards.
struct Scratch {
  std::filesystem::path dir;
  explicit Scratch(const std::string& name)
      : dir(std::filesystem::path(::testing::TempDir()) /
            ("sun_input_hash_" + name + "_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~Scratch() { std::filesystem::remove_all(dir); }
  std::string path(const std::string& name) const {
    return (dir / name).string();
  }
  // Run build/sun with `arguments`; returns what it printed.
  std::string runSun(const std::string& arguments) const {
    const std::string cmd =
        "build/sun " + arguments + " > " + path("log") + " 2>&1";
    int rc = std::system(cmd.c_str());
    EXPECT_EQ(WEXITSTATUS(rc), 0) << readFile(path("log"));
    return readFile(path("log"));
  }
};

bool haveSunBinary() { return std::filesystem::exists("build/sun"); }

sun::BuildInputs makeInputs() {
  sun::BuildInputs inputs;
  inputs.artifactKind = "executable";
  inputs.sourceDigests = {"aaa", "bbb"};
  inputs.archives = {{"libx.a", "ccc"}};
  inputs.settings = {{"library", "m"}};
  return inputs;
}

}  // namespace

// ============================================================================
// The hash
// ============================================================================

TEST(Tooling_Backend_InputHash, same_inputs_same_hash) {
  EXPECT_EQ(sun::computeInputHash(makeInputs()),
            sun::computeInputHash(makeInputs()));
}

TEST(Tooling_Backend_InputHash, source_order_does_not_matter) {
  auto reordered = makeInputs();
  reordered.sourceDigests = {"bbb", "aaa"};
  EXPECT_EQ(sun::computeInputHash(makeInputs()),
            sun::computeInputHash(reordered));
}

TEST(Tooling_Backend_InputHash, every_input_changes_the_hash) {
  const std::string base = sun::computeInputHash(makeInputs());

  auto changed = makeInputs();
  changed.artifactKind = "tests";
  EXPECT_NE(sun::computeInputHash(changed), base);

  changed = makeInputs();
  changed.sourceDigests[0] = "aab";
  EXPECT_NE(sun::computeInputHash(changed), base);

  changed = makeInputs();
  changed.archives[0].second = "ccd";
  EXPECT_NE(sun::computeInputHash(changed), base);

  changed = makeInputs();
  changed.targetTriple = "aarch64-unknown-linux-gnu";
  EXPECT_NE(sun::computeInputHash(changed), base);

  changed = makeInputs();
  changed.debugInfo = true;
  EXPECT_NE(sun::computeInputHash(changed), base);

  changed = makeInputs();
  changed.optimize = false;
  EXPECT_NE(sun::computeInputHash(changed), base);

  changed = makeInputs();
  changed.settings[0].second = "z";
  EXPECT_NE(sun::computeInputHash(changed), base);
}

// A value cannot slide from one field into its neighbour and hash the same.
TEST(Tooling_Backend_InputHash, fields_do_not_run_together) {
  auto left = makeInputs();
  left.settings = {{"library", "ab"}, {"library", "c"}};
  auto right = makeInputs();
  right.settings = {{"library", "a"}, {"library", "bc"}};
  EXPECT_NE(sun::computeInputHash(left), sun::computeInputHash(right));
}

TEST(Tooling_Backend_InputHash, compiler_digest_is_stable) {
  EXPECT_EQ(sun::getCompilerDigest().size(), 64u);
  EXPECT_EQ(sun::getCompilerDigest(), sun::getCompilerDigest());
}

TEST(Tooling_Backend_InputHash, missing_artifacts_have_no_record) {
  EXPECT_FALSE(sun::readBuildRecord("/nonexistent/app").has_value());
  EXPECT_FALSE(sun::readMoonInputHash("/nonexistent/lib.moon").has_value());
  // A file that is not an object file carries no record either
  Scratch scratch("not_object");
  writeFile(scratch.path("notes.txt"), "plain text\n");
  EXPECT_FALSE(sun::readBuildRecord(scratch.path("notes.txt")).has_value());
}

// ============================================================================
// End to end, through the sun binary
// ============================================================================

// A bundle is rebuilt when a source named by its manifest changes, and left
// alone otherwise. Without the flag it is rebuilt regardless.
TEST(Tooling_Backend_InputHash, bundle_is_rebuilt_only_when_inputs_change) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  Scratch scratch("moon");
  writeFile(scratch.path("util.sun"), R"(
public module input_hash_lib {
  public function two() i32 { return 2; }
}
)");
  writeFile(scratch.path("lib.sun"), R"(
public module input_hash_lib {
  public function one() i32 { return 1; }
}
manifest { source_files: ["util.sun"] }
)");
  const std::string moon = scratch.path("lib.moon");
  const std::string always =
      "--emit-moon -o " + moon + " " + scratch.path("lib.sun");
  const std::string build = "--skip-if-unchanged " + always;

  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully created"));
  const auto firstHash = sun::readMoonInputHash(moon);
  ASSERT_TRUE(firstHash.has_value());

  const auto written = std::filesystem::last_write_time(moon);
  EXPECT_TRUE(contains(scratch.runSun(build), "Up to date: " + moon));
  EXPECT_EQ(std::filesystem::last_write_time(moon), written);

  EXPECT_TRUE(contains(scratch.runSun(always), "Successfully created"));
  EXPECT_EQ(sun::readMoonInputHash(moon), firstHash);

  writeFile(scratch.path("util.sun"), R"(
public module input_hash_lib {
  public function two() i32 { return 22; }
}
)");
  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully created"));
  EXPECT_NE(sun::readMoonInputHash(moon), firstHash);
}

// Flags are inputs too: the same sources with -g are a different bundle.
TEST(Tooling_Backend_InputHash, flags_are_inputs) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  Scratch scratch("flags");
  writeFile(scratch.path("lib.sun"), R"(
public module input_hash_flags {
  public function one() i32 { return 1; }
}
)");
  const std::string build = "--skip-if-unchanged --emit-moon -o " +
                            scratch.path("lib.moon") + " " +
                            scratch.path("lib.sun");
  scratch.runSun(build);
  EXPECT_TRUE(contains(scratch.runSun("-g " + build), "Successfully created"));
  EXPECT_TRUE(contains(scratch.runSun("-g " + build), "Up to date"));
}

// -c on a program with tests: a change to a test file rebuilds the test
// binary and leaves the executable alone, and a change to the program
// rebuilds both.
TEST(Tooling_Backend_InputHash, program_and_tests_are_skipped_separately) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  if (!std::filesystem::exists("build/stdlib.moon")) {
    GTEST_SKIP() << "build/stdlib.moon not found";
  }
  Scratch scratch("compile");
  const std::string tests = R"(
using std;
module input_hash_app {
  test_function answer_is_forty_two() {
    std.test.assert_eq(input_hash_app.answer(), 42);
  }
}
)";
  const std::string program = R"(
using std;
module input_hash_app {
  function answer() i32 { return 42; }
}
function main() i32 { return 0; }
manifest {
  test_files: ["app_tests.sun"]
  libraries: ["stdlib.moon"]
}
)";
  writeFile(scratch.path("app_tests.sun"), tests);
  writeFile(scratch.path("app.sun"), program);
  const std::string app = scratch.path("app");
  const std::string build = "-c --skip-if-unchanged --lib-path build -o " +
                            app + " " + scratch.path("app.sun");

  std::string log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Successfully compiled to: " + app)) << log;
  EXPECT_TRUE(contains(log, "Successfully compiled test binary to")) << log;

  auto record = sun::readBuildRecord(app);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->hasTests);
  auto testRecord = sun::readBuildRecord(app + "_test");
  ASSERT_TRUE(testRecord.has_value());
  EXPECT_TRUE(testRecord->hasExecutable);
  EXPECT_NE(record->inputHash, testRecord->inputHash);

  log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Up to date: " + app + "\n")) << log;
  EXPECT_TRUE(contains(log, "Up to date: " + app + "_test")) << log;
  EXPECT_FALSE(contains(log, "Successfully compiled")) << log;

  writeFile(scratch.path("app_tests.sun"), tests + "// edited\n");
  log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Up to date: " + app + "\n")) << log;
  EXPECT_TRUE(contains(log, "Successfully compiled test binary to")) << log;

  writeFile(scratch.path("app.sun"), program + "// edited\n");
  log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Successfully compiled to: " + app)) << log;
  EXPECT_TRUE(contains(log, "Successfully compiled test binary to")) << log;

  // The rebuilt executable still runs
  EXPECT_EQ(WEXITSTATUS(std::system(app.c_str())), 0);
}

// A program without tests records that, so a later run does not compile it
// again just to find out there is no test binary to build.
TEST(Tooling_Backend_InputHash, program_without_tests_is_skipped) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  Scratch scratch("no_tests");
  writeFile(scratch.path("app.sun"), "function main() i32 { return 0; }\n");
  const std::string app = scratch.path("app");
  const std::string build =
      "-c --skip-if-unchanged -o " + app + " " + scratch.path("app.sun");

  scratch.runSun(build);
  auto record = sun::readBuildRecord(app);
  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->hasTests);

  const std::string log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Up to date: " + app)) << log;
  EXPECT_FALSE(contains(log, "Compiling:")) << log;

  // A deleted artifact is rebuilt even though nothing else changed
  std::filesystem::remove(app);
  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully compiled to"));
}

// Skipping is opt-in: without the flag every run builds, and the executable
// carries no record of its inputs.
TEST(Tooling_Backend_InputHash, nothing_is_skipped_or_recorded_by_default) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  Scratch scratch("default");
  writeFile(scratch.path("app.sun"), "function main() i32 { return 0; }\n");
  const std::string app = scratch.path("app");
  const std::string build = "-c -o " + app + " " + scratch.path("app.sun");

  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully compiled to"));
  EXPECT_FALSE(sun::readBuildRecord(app).has_value());
  const std::string log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Successfully compiled to")) << log;
  EXPECT_FALSE(contains(log, "Up to date")) << log;

  // An executable built without a record is rebuilt once skipping is asked
  // for, and only then left alone
  const std::string skipping = "--skip-if-unchanged " + build;
  EXPECT_TRUE(contains(scratch.runSun(skipping), "Successfully compiled to"));
  EXPECT_TRUE(contains(scratch.runSun(skipping), "Up to date: " + app));
}

// The flag is about built artifacts, so it has nothing to say about a JIT
// run.
TEST(Tooling_Backend_InputHash, flag_is_rejected_without_a_build_mode) {
  if (!haveSunBinary()) GTEST_SKIP() << "build/sun not found";
  Scratch scratch("jit");
  writeFile(scratch.path("run.sun"), "function main() i32 { return 0; }\n");
  const std::string cmd = "build/sun --skip-if-unchanged " +
                          scratch.path("run.sun") + " > " +
                          scratch.path("log") + " 2>&1";
  int rc = std::system(cmd.c_str());
  EXPECT_NE(WEXITSTATUS(rc), 0);
  EXPECT_TRUE(contains(readFile(scratch.path("log")), "--skip-if-unchanged"));
}
