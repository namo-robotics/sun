// tests/tooling/backend/test_input_hash.cpp - Skipping builds by input hash
//
// By default, every artifact records the hash of the inputs it
// was built from, and a run that arrives at the same hash leaves the
// artifact alone. The hash is checked directly; the end-to-end cases drive
// the sun binary the way a build tool running it unconditionally would, and
// look at what it rebuilt.

#include <gtest/gtest.h>
#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "abi/abi_compatibility_checker.h"
#include "driver/build_record.h"
#include "driver/input_hash.h"
#include "moon_bundling/moon.h"

/** Keeps test fixtures and helpers local to this source file. */
namespace {

/** Reads a fixture file into a string for comparison. */
std::string readFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/** Writes source or fixture data to a test file. */
void writeFile(const std::string& path, const std::string& text) {
  std::ofstream(path) << text;
}

/** Reports whether the expected text occurs in the captured output. */
bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

/**
 * A scratch folder for one end-to-end case, removed afterwards.
 */
struct Scratch {
  std::filesystem::path dir;
  /** Owns a temporary directory used for build-provenance test artifacts. */
  explicit Scratch(const std::string& name)
      : dir(std::filesystem::path(::testing::TempDir()) /
            ("sun_input_hash_" + name + "_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  /** Removes the temporary build-provenance test directory. */
  ~Scratch() { std::filesystem::remove_all(dir); }
  /** Returns a filename within the scratch directory. */
  std::string path(const std::string& name) const {
    return (dir / name).string();
  }
  /**
   * Runs build/sun with progress logging so cache decisions can be asserted.
   */
  std::string runSun(const std::string& arguments) const {
    const std::string cmd = "SUN_LOG_LEVEL=info build/sun " + arguments +
                            " > " + path("log") + " 2>&1";
    int rc = std::system(cmd.c_str());
    EXPECT_EQ(WEXITSTATUS(rc), 0) << readFile(path("log"));
    return readFile(path("log"));
  }
};

/** Reports whether the compiler executable required by this test is available.
 */
bool haveSunBinary() { return std::filesystem::exists("build/sun"); }

/** Builds the input description used to check deterministic build hashes. */
sun::driver::BuildInputs makeInputs() {
  sun::driver::BuildInputs inputs;
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

/** Hashing prepared text does not need a schema or parse the generated source.
 */
TEST(Tooling_Backend_InputHash, hashes_prepared_proto_text_without_generation) {
  std::vector<sun::moon_bundling::SynthesizedProtoModule> modules = {
      {"not valid Sun source", "<proto:missing.proto>", "prepared"}};
  sun::driver::BuildInputs original;
  ASSERT_NO_THROW(sun::driver::addSourceDigests(original, {}, modules));
  ASSERT_EQ(original.sourceDigests.size(), 1u);

  modules[0].pseudoPath = "<proto:renamed.proto>";
  sun::driver::BuildInputs renamed;
  sun::driver::addSourceDigests(renamed, {}, modules);
  EXPECT_EQ(original.sourceDigests, renamed.sourceDigests);

  modules[0].sunSource += " changed";
  sun::driver::BuildInputs changed;
  sun::driver::addSourceDigests(changed, {}, modules);
  EXPECT_NE(original.sourceDigests, changed.sourceDigests);
}

TEST(Tooling_Backend_InputHash, same_inputs_same_hash) {
  EXPECT_EQ(sun::driver::computeInputHash(makeInputs()),
            sun::driver::computeInputHash(makeInputs()));
}

TEST(Tooling_Backend_InputHash, source_order_does_not_matter) {
  auto reordered = makeInputs();
  reordered.sourceDigests = {"bbb", "aaa"};
  EXPECT_EQ(sun::driver::computeInputHash(makeInputs()),
            sun::driver::computeInputHash(reordered));
}

TEST(Tooling_Backend_InputHash, every_input_changes_the_hash) {
  const std::string base = sun::driver::computeInputHash(makeInputs());

  auto changed = makeInputs();
  changed.artifactKind = "tests";
  EXPECT_NE(sun::driver::computeInputHash(changed), base);

  changed = makeInputs();
  changed.sourceDigests[0] = "aab";
  EXPECT_NE(sun::driver::computeInputHash(changed), base);

  changed = makeInputs();
  changed.archives[0].second = "ccd";
  EXPECT_NE(sun::driver::computeInputHash(changed), base);

  changed = makeInputs();
  changed.targetTriple = "aarch64-unknown-linux-gnu";
  EXPECT_NE(sun::driver::computeInputHash(changed), base);

  changed = makeInputs();
  changed.debugInfo = true;
  EXPECT_NE(sun::driver::computeInputHash(changed), base);

  changed = makeInputs();
  changed.optimize = false;
  EXPECT_NE(sun::driver::computeInputHash(changed), base);

  changed = makeInputs();
  changed.settings[0].second = "z";
  EXPECT_NE(sun::driver::computeInputHash(changed), base);
}

// A value cannot slide from one field into its neighbour and hash the same.
TEST(Tooling_Backend_InputHash, fields_do_not_run_together) {
  auto left = makeInputs();
  left.settings = {{"library", "ab"}, {"library", "c"}};
  auto right = makeInputs();
  right.settings = {{"library", "a"}, {"library", "bc"}};
  EXPECT_NE(sun::driver::computeInputHash(left),
            sun::driver::computeInputHash(right));
}

TEST(Tooling_Backend_InputHash, compiler_digest_is_stable) {
  EXPECT_EQ(sun::driver::getCompilerDigest().size(), 64u);
  EXPECT_EQ(sun::driver::getCompilerDigest(), sun::driver::getCompilerDigest());
}

TEST(Tooling_Backend_InputHash, missing_artifacts_have_no_record) {
  EXPECT_FALSE(sun::driver::readBuildRecord("/nonexistent/app").has_value());
  EXPECT_FALSE(
      sun::driver::readMoonInputHash("/nonexistent/lib.moon").has_value());
  /**
   * A file that is not an object file carries no record either
   */
  Scratch scratch("not_object");
  writeFile(scratch.path("notes.txt"), "plain text\n");
  EXPECT_FALSE(
      sun::driver::readBuildRecord(scratch.path("notes.txt")).has_value());
}

// ============================================================================
// End to end, through the sun binary
// ============================================================================

// A bundle is rebuilt when a source named by its manifest changes, and left
// alone otherwise. Without the flag it is rebuilt regardless.
TEST(Tooling_Backend_InputHash, bundle_is_rebuilt_only_when_inputs_change) {
  if (!haveSunBinary()) {
    GTEST_SKIP() << "build/sun not found";
  }
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
  const std::string build = always;
  const std::string forced = "--force-rebuild " + always;

  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully created"));
  const auto firstHash = sun::driver::readMoonInputHash(moon);
  ASSERT_TRUE(firstHash.has_value());

  const auto written = std::filesystem::last_write_time(moon);
  EXPECT_TRUE(contains(scratch.runSun(build), "Up to date: " + moon));
  EXPECT_EQ(std::filesystem::last_write_time(moon), written);

  EXPECT_TRUE(contains(scratch.runSun(forced), "Successfully created"));
  EXPECT_EQ(sun::driver::readMoonInputHash(moon), firstHash);

  writeFile(scratch.path("util.sun"), R"(
public module input_hash_lib {
  public function two() i32 { return 22; }
}
)");
  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully created"));
  EXPECT_NE(sun::driver::readMoonInputHash(moon), firstHash);
}

// Flags are inputs too: the same sources with -g are a different bundle.
TEST(Tooling_Backend_InputHash, flags_are_inputs) {
  if (!haveSunBinary()) {
    GTEST_SKIP() << "build/sun not found";
  }
  Scratch scratch("flags");
  writeFile(scratch.path("lib.sun"), R"(
public module input_hash_flags {
  public function one() i32 { return 1; }
}
)");
  const std::string build = "--emit-moon -o " + scratch.path("lib.moon") + " " +
                            scratch.path("lib.sun");
  scratch.runSun(build);
  EXPECT_TRUE(contains(scratch.runSun("-g " + build), "Successfully created"));
  EXPECT_TRUE(contains(scratch.runSun("-g " + build), "Up to date"));
}

// -c on a program with tests: a change to a test file rebuilds the test
// binary and leaves the executable alone, and a change to the program
// rebuilds both.
TEST(Tooling_Backend_InputHash, program_and_tests_are_skipped_separately) {
  if (!haveSunBinary()) {
    GTEST_SKIP() << "build/sun not found";
  }
  if (!std::filesystem::exists("build/stdlib.moon")) {
    GTEST_SKIP() << "build/stdlib.moon not found";
  }
  Scratch scratch("compile");
  const std::string tests = R"(
using std;
module input_hash_app {
  test_function answer_is_forty_two() {
    try std.test.assert_eq(input_hash_app.answer(), 42);
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
  const std::string build =
      "-c --lib-path build -o " + app + " " + scratch.path("app.sun");

  std::string log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Successfully compiled to: " + app)) << log;
  EXPECT_TRUE(contains(log, "Successfully compiled test binary to")) << log;

  auto record = sun::driver::readBuildRecord(app);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->hasTests);
  auto testRecord = sun::driver::readBuildRecord(app + "_test");
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
  int status = std::system(app.c_str());
  ASSERT_NE(status, -1);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

// A program without tests records that, so a later run does not compile it
// again just to find out there is no test binary to build.
TEST(Tooling_Backend_InputHash, program_without_tests_is_skipped) {
  if (!haveSunBinary()) {
    GTEST_SKIP() << "build/sun not found";
  }
  Scratch scratch("no_tests");
  writeFile(scratch.path("app.sun"), "function main() i32 { return 0; }\n");
  const std::string app = scratch.path("app");
  const std::string build = "-c -o " + app + " " + scratch.path("app.sun");

  scratch.runSun(build);
  auto record = sun::driver::readBuildRecord(app);
  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->hasTests);

  const std::string log = scratch.runSun(build);
  EXPECT_TRUE(contains(log, "Up to date: " + app)) << log;
  EXPECT_FALSE(contains(log, "Compiling:")) << log;

  // A deleted artifact is rebuilt even though nothing else changed
  std::filesystem::remove(app);
  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully compiled to"));
}

// Skipping is the default: the second identical build does nothing, and
// --force-rebuild builds anyway while still recording the inputs, so the run
// after it skips again.
TEST(Tooling_Backend_InputHash, force_rebuild_builds_and_still_records) {
  if (!haveSunBinary()) {
    GTEST_SKIP() << "build/sun not found";
  }
  Scratch scratch("force");
  writeFile(scratch.path("app.sun"), "function main() i32 { return 0; }\n");
  const std::string app = scratch.path("app");
  const std::string build = "-c -o " + app + " " + scratch.path("app.sun");

  EXPECT_TRUE(contains(scratch.runSun(build), "Successfully compiled to"));
  ASSERT_TRUE(sun::driver::readBuildRecord(app).has_value());
  EXPECT_TRUE(contains(scratch.runSun(build), "Up to date: " + app));

  const std::string forced = "--force-rebuild " + build;
  std::string log = scratch.runSun(forced);
  EXPECT_TRUE(contains(log, "Successfully compiled to")) << log;
  EXPECT_FALSE(contains(log, "Up to date")) << log;
  EXPECT_TRUE(sun::driver::readBuildRecord(app).has_value());
  EXPECT_TRUE(contains(scratch.runSun(build), "Up to date: " + app));
}

// The flag is about built artifacts, so it has nothing to say about a JIT
// run.
TEST(Tooling_Backend_InputHash, flag_is_rejected_without_a_build_mode) {
  if (!haveSunBinary()) {
    GTEST_SKIP() << "build/sun not found";
  }
  Scratch scratch("jit");
  writeFile(scratch.path("run.sun"), "function main() i32 { return 0; }\n");
  const std::string cmd = "build/sun --force-rebuild " +
                          scratch.path("run.sun") + " > " +
                          scratch.path("log") + " 2>&1";
  int rc = std::system(cmd.c_str());
  EXPECT_NE(WEXITSTATUS(rc), 0);
  EXPECT_TRUE(contains(readFile(scratch.path("log")), "--force-rebuild"));
}

/** Names change artifact identity while preserving ABI and import
 * compatibility. */
TEST(Tooling_Backend_InputHash, bundle_version_names_are_only_artifact_inputs) {
  if (!haveSunBinary()) {
    GTEST_SKIP();
  }
  Scratch scratch("version_names");
  writeFile(scratch.path("lib.sun"),
            "public module version_names { public function answer() i32 { "
            "return 42; } }"
            " manifest { library_id: \"test/version-names\" }");
  const auto output = scratch.path("lib.moon");
  const auto command =
      "--emit-moon -o " + output + " " + scratch.path("lib.sun");
  scratch.runSun(command);
  auto unnamed = sun::moon_bundling::MoonReader::open(output);
  ASSERT_TRUE(unnamed);
  EXPECT_FALSE(unnamed->getLibrary().has_version_name());
  const auto hash = unnamed->getLibrary().content_hash();
  const auto abiHash =
      unnamed->getLibrary().bundle_contract().library_abi().abi_hash();
  ASSERT_FALSE(abiHash.empty());
  scratch.runSun(command + " --version-name 'release candidate'");
  auto named = sun::moon_bundling::MoonReader::open(output);
  ASSERT_TRUE(named);
  EXPECT_EQ(named->getLibrary().version_name(), "release candidate");
  EXPECT_NE(named->getLibrary().content_hash(), hash);
  EXPECT_EQ(named->getLibrary().bundle_contract().library_abi().abi_hash(),
            abiHash);
  auto& checker = sun::abi::AbiCompatibilityChecker::current();
  EXPECT_NO_THROW(checker.validate(*unnamed, unnamed->getTargetTriple()));
  EXPECT_NO_THROW(checker.validate(*named, named->getTargetTriple()));
  EXPECT_TRUE(
      contains(scratch.runSun(command + " --version-name 'release candidate'"),
               "Up to date:"));
  ASSERT_TRUE(named->writeDebugJson(scratch.path("moon.json")));
  EXPECT_TRUE(
      contains(readFile(scratch.path("moon.json")), "release candidate"));
  scratch.runSun(command + " --version-name ''");
  auto empty = sun::moon_bundling::MoonReader::open(output);
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty->getLibrary().has_version_name());
  EXPECT_EQ(empty->getLibrary().content_hash(), hash);
}

/** A config invocation gives each built library the requested name. */
TEST(Tooling_Backend_InputHash, config_version_name_reaches_each_library) {
  if (!haveSunBinary()) {
    GTEST_SKIP();
  }
  Scratch scratch("config_version_names");
  writeFile(scratch.path("dep.sun"),
            "public module dep { public function value() i32 { return 3; } }");
  scratch.runSun("--emit-moon " + scratch.path("dep.sun") + " -o " +
                 scratch.path("dep.moon") + " --version-name dependency");
  const auto dependencyBytes = readFile(scratch.path("dep.moon"));
  writeFile(scratch.path("one.sun"),
            "public module one { public function value() i32 { return "
            "dep.value(); } }"
            " manifest { libraries: [\"dep.moon\"] }");
  writeFile(scratch.path("two.sun"),
            "public module two { public function value() i32 { return 2; } }");
  writeFile(scratch.path("sun-config.json"), R"({
    "root": true,
    "entrypoints": [
      {"path": "one.sun", "type": "library", "output_name": "one"},
      {"path": "two.sun", "type": "library", "output_name": "two"}
    ]
  })");
  scratch.runSun("-c --no-test " + scratch.path("sun-config.json") +
                 " --version-name v1.0.0");
  EXPECT_EQ(readFile(scratch.path("dep.moon")), dependencyBytes);
  for (const auto* file : {"one.moon", "two.moon"}) {
    auto reader = sun::moon_bundling::MoonReader::open(scratch.path(file));
    ASSERT_TRUE(reader);
    EXPECT_EQ(reader->getLibrary().version_name(), "v1.0.0");
  }
}
