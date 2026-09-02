// tests/modules/test_manifest_test_files.cpp - The manifest's test_files:
// list: test-only sources, resolved like source_files but kept apart so
// only test builds merge them into the program.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "driver/manifest_processor.h"

namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& name) {
  fs::path dir = fs::temp_directory_path() / "sun_manifest_test_files" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void writeFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

bool includes(const std::vector<std::string>& files, const std::string& name) {
  for (const auto& f : files) {
    if (fs::path(f).filename() == name) return true;
  }
  return false;
}

}  // namespace

TEST(Modules_ManifestTestFiles, test_files_stay_out_of_source_files) {
  fs::path dir = freshDir("top_level");
  writeFile(dir / "lib.sun", "public module m {}\n");
  writeFile(dir / "lib_tests.sun", "public module m {}\n");
  writeFile(dir / "main.sun",
            "manifest {\n"
            "    source_files: [ \"lib.sun\" ],\n"
            "    test_files: [ \"lib_tests.sun\" ]\n"
            "}\n"
            "function main() i32 { return 0; }\n");

  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
  ASSERT_TRUE(resolved.has_value());
  EXPECT_TRUE(includes(resolved->sunFiles, "lib.sun"));
  EXPECT_FALSE(includes(resolved->sunFiles, "lib_tests.sun"));
  EXPECT_TRUE(includes(resolved->testSunFiles, "lib_tests.sun"));
  EXPECT_FALSE(includes(resolved->testSunFiles, "lib.sun"));
}

TEST(Modules_ManifestTestFiles, target_blocks_take_test_files_too) {
  fs::path dir = freshDir("per_target");
  writeFile(dir / "linux_tests.sun", "public module m {}\n");
  writeFile(dir / "darwin_tests.sun", "public module m {}\n");
  writeFile(dir / "main.sun",
            "manifest {\n"
            "    target: {\n"
            "        linux: {\n"
            "            test_files: [ \"linux_tests.sun\" ]\n"
            "        },\n"
            "        macos: {\n"
            "            test_files: [ \"darwin_tests.sun\" ]\n"
            "        }\n"
            "    }\n"
            "}\n"
            "function main() i32 { return 0; }\n");

  auto forLinux = sun::ManifestProcessor::fromEntrypointFile(
      (dir / "main.sun").string(), "x86_64-linux-gnu");
  ASSERT_TRUE(forLinux.has_value());
  EXPECT_TRUE(includes(forLinux->testSunFiles, "linux_tests.sun"));
  EXPECT_FALSE(includes(forLinux->testSunFiles, "darwin_tests.sun"));
  EXPECT_TRUE(forLinux->sunFiles.empty());
}
