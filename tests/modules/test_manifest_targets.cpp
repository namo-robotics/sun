// tests/modules/test_manifest_targets.cpp - The manifest's target: block:
// sources and archives that only apply when compiling for one OS. The
// manifest never decides the target — the compilation's --target (or the
// host) does — a block only says which entries belong to builds for that OS.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "driver/manifest_processor.h"
#include "parsing/parser.h"
#include "support/error.h"
#include "support/target_os.h"

namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& name) {
  fs::path dir = fs::temp_directory_path() / "sun_manifest_target_tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void writeFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

// An entrypoint whose manifest carries one shared file and one file per OS.
fs::path writeTargetedManifest(const std::string& name) {
  fs::path dir = freshDir(name);
  writeFile(dir / "shared.sun", "public module m {}\n");
  writeFile(dir / "linux_only.sun", "public module m {}\n");
  writeFile(dir / "darwin_only.sun", "public module m {}\n");
  writeFile(dir / "main.sun",
            "manifest {\n"
            "    source_files: [ \"shared.sun\" ],\n"
            "    target: {\n"
            "        linux: {\n"
            "            source_files: [ \"linux_only.sun\" ]\n"
            "        },\n"
            "        macos: {\n"
            "            source_files: [ \"darwin_only.sun\" ]\n"
            "        }\n"
            "    }\n"
            "}\n"
            "function main() i32 { return 0; }\n");
  return dir;
}

bool includes(const std::vector<std::string>& files, const std::string& name) {
  for (const auto& f : files) {
    if (fs::path(f).filename() == name) return true;
  }
  return false;
}

}  // namespace

TEST(Modules_ManifestTargets, target_block_follows_the_compilation_target) {
  fs::path dir = writeTargetedManifest("per_target");
  std::string entry = (dir / "main.sun").string();

  auto forLinux =
      sun::ManifestProcessor::fromEntrypointFile(entry, "aarch64-linux-gnu");
  ASSERT_TRUE(forLinux.has_value());
  EXPECT_TRUE(includes(forLinux->sunFiles, "shared.sun"));
  EXPECT_TRUE(includes(forLinux->sunFiles, "linux_only.sun"));
  EXPECT_FALSE(includes(forLinux->sunFiles, "darwin_only.sun"));

  auto forDarwin =
      sun::ManifestProcessor::fromEntrypointFile(entry, "arm64-apple-darwin");
  ASSERT_TRUE(forDarwin.has_value());
  EXPECT_TRUE(includes(forDarwin->sunFiles, "shared.sun"));
  EXPECT_TRUE(includes(forDarwin->sunFiles, "darwin_only.sun"));
  EXPECT_FALSE(includes(forDarwin->sunFiles, "linux_only.sun"));
}

TEST(Modules_ManifestTargets, target_sources_come_before_the_shared_list) {
  // Per-OS files define the primitives shared code consumes (errno, socket
  // options), and later files may reference earlier ones but not the other
  // way round — so the matching block's sources lead.
  fs::path dir = writeTargetedManifest("ordering");
  auto resolved = sun::ManifestProcessor::fromEntrypointFile(
      (dir / "main.sun").string(), "x86_64-linux-gnu");
  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->sunFiles.size(), 2u);
  EXPECT_EQ(fs::path(resolved->sunFiles[0]).filename(), "linux_only.sun");
  EXPECT_EQ(fs::path(resolved->sunFiles[1]).filename(), "shared.sun");
}

TEST(Modules_ManifestTargets, empty_triple_selects_the_host_os) {
  fs::path dir = writeTargetedManifest("host_default");
  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
  ASSERT_TRUE(resolved.has_value());

  auto hostOs = sun::targetOsName(sun::resolvedTargetTriple(""));
  ASSERT_TRUE(hostOs.has_value());
  EXPECT_EQ(includes(resolved->sunFiles, "linux_only.sun"), *hostOs == "linux");
  EXPECT_EQ(includes(resolved->sunFiles, "darwin_only.sun"),
            *hostOs == "macos");
}

TEST(Modules_ManifestTargets, unknown_os_name_is_a_parse_error) {
  auto parser = Parser::createStringParser(
      "manifest {\n"
      "    target: {\n"
      "        maos: { source_files: [ \"x.sun\" ] }\n"
      "    }\n"
      "}\n");
  EXPECT_THROW(parser.parseProgram(), SunError);
}

TEST(Modules_ManifestTargets, target_blocks_may_carry_archives) {
  fs::path dir = freshDir("archives");
  writeFile(dir / "libmac.a", "not a real archive\n");
  writeFile(dir / "main.sun",
            "manifest {\n"
            "    source_files: [],\n"
            "    target: {\n"
            "        macos: {\n"
            "            archives: [ \"libmac.a\" ]\n"
            "        }\n"
            "    }\n"
            "}\n");

  auto forDarwin = sun::ManifestProcessor::fromEntrypointFile(
      (dir / "main.sun").string(), "arm64-apple-darwin");
  ASSERT_TRUE(forDarwin.has_value());
  ASSERT_EQ(forDarwin->archiveFiles.size(), 1u);
  EXPECT_EQ(fs::path(forDarwin->archiveFiles[0]).filename(), "libmac.a");

  auto forLinux = sun::ManifestProcessor::fromEntrypointFile(
      (dir / "main.sun").string(), "x86_64-linux-gnu");
  ASSERT_TRUE(forLinux.has_value());
  EXPECT_TRUE(forLinux->archiveFiles.empty());
}
