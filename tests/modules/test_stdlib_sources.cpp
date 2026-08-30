// tests/modules/test_stdlib_sources.cpp - The stdlib file list is written in
// two places that must agree: the manifest in stdlib/stdlib.sun, which the
// compiler reads when it bundles stdlib.moon, and STDLIB_SOURCES in
// CMakeLists.txt, which is the dependency list that decides when the bundle
// is rebuilt.
//
// A file present in the manifest and absent from STDLIB_SOURCES builds
// correctly once — the entrypoint itself changed — and is then frozen:
// later edits to it never rebuild the bundle, so programs keep linking
// against the older copy. Nothing errors, and the symptom looks like a
// compiler bug rather than a stale artifact.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

namespace {

// The workspace root, which SUN_PATH points at while the tests run.
std::filesystem::path workspaceRoot() {
  initTestEnvironment();
  const char* sunPath = std::getenv("SUN_PATH");
  if (sunPath && std::filesystem::exists(std::filesystem::path(sunPath) /
                                         "CMakeLists.txt")) {
    return std::filesystem::path(sunPath);
  }
  return std::filesystem::current_path();
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Every "name.sun" quoted in stdlib/stdlib.sun, which covers the shared
// source_files list and the per-OS target blocks alike.
std::set<std::string> manifestSources(const std::filesystem::path& root) {
  std::set<std::string> names;
  const std::string text = readFile(root / "stdlib" / "stdlib.sun");
  std::regex pattern(R"RX("([A-Za-z0-9_]+\.sun)")RX");
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end;
       it != end; ++it) {
    names.insert((*it)[1].str());
  }
  return names;
}

// Every stdlib/name.sun named in the STDLIB_SOURCES block of CMakeLists.txt.
std::set<std::string> cmakeSources(const std::filesystem::path& root) {
  std::set<std::string> names;
  const std::string text = readFile(root / "CMakeLists.txt");
  std::smatch block;
  std::regex blockPattern(R"(set\(STDLIB_SOURCES([\s\S]*?)\n\s*\)\n)");
  if (!std::regex_search(text, block, blockPattern)) return names;
  const std::string body = block[1].str();
  std::regex pattern(R"(stdlib/([A-Za-z0-9_]+\.sun))");
  for (std::sregex_iterator it(body.begin(), body.end(), pattern), end;
       it != end; ++it) {
    names.insert((*it)[1].str());
  }
  return names;
}

}  // namespace

// The parsing above has to actually find something, or the comparison below
// would pass by matching two empty sets.
TEST(Modules_StdlibSources, both_lists_are_found_and_non_empty) {
  const auto root = workspaceRoot();
  EXPECT_GT(manifestSources(root).size(), 10u)
      << "no source_files parsed out of stdlib/stdlib.sun";
  EXPECT_GT(cmakeSources(root).size(), 10u)
      << "no STDLIB_SOURCES block parsed out of CMakeLists.txt";
}

TEST(Modules_StdlibSources, every_manifest_file_is_a_build_dependency) {
  const auto root = workspaceRoot();
  const auto manifest = manifestSources(root);
  const auto cmake = cmakeSources(root);

  for (const auto& name : manifest) {
    EXPECT_TRUE(cmake.count(name) > 0)
        << "stdlib/" << name
        << " is in the manifest but not in STDLIB_SOURCES, so editing it will "
           "not rebuild stdlib.moon. Add it to CMakeLists.txt.";
  }
}

// The other direction is only stale bookkeeping — a dependency on a file the
// bundle does not include costs a needless rebuild, never a wrong one — so it
// is worth reporting without failing the build.
TEST(Modules_StdlibSources, build_dependencies_that_no_longer_ship) {
  const auto root = workspaceRoot();
  const auto manifest = manifestSources(root);
  for (const auto& name : cmakeSources(root)) {
    if (manifest.count(name) == 0) {
      GTEST_LOG_(WARNING) << "stdlib/" << name
                          << " is in STDLIB_SOURCES but not in the manifest";
    }
  }
  SUCCEED();
}
