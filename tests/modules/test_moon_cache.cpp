// tests/modules/test_moon_cache.cpp - Download cache for url moon
// dependencies. All tests are offline: file:// URLs plus pre-seeded caches.

#include <gtest/gtest.h>
#include <llvm/Support/SHA256.h>

#include <filesystem>
#include <fstream>

#include "moon_bundling/moon_cache.h"
#include "support/error.h"

namespace fs = std::filesystem;

namespace {

std::string sha256Hex(const std::string& content) {
  llvm::SHA256 sha;
  sha.update(llvm::StringRef(content));
  auto bytes = sha.final();
  std::string hex;
  for (uint8_t b : bytes) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", b);
    hex += buf;
  }
  return hex;
}

// Fresh source + cache directories under ${workspaceRoot}/tmp
struct CacheFixture {
  fs::path root;
  fs::path srcDir;
  fs::path cacheDir;

  explicit CacheFixture(const std::string& name) {
    root = fs::path("tmp") / "moon_cache_tests" / name;
    fs::remove_all(root);
    srcDir = root / "src";
    cacheDir = root / "cache";
    fs::create_directories(srcDir);
    fs::create_directories(cacheDir);
  }

  std::string writeSource(const std::string& filename,
                          const std::string& content) {
    fs::path file = srcDir / filename;
    std::ofstream out(file, std::ios::binary);
    out << content;
    out.close();
    return "file://" + fs::absolute(file).string();
  }
};

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

TEST(Modules_MoonCache, fetch_downloads_file_url_and_caches) {
  CacheFixture fx("download_and_cache");
  std::string url = fx.writeSource("lib.moon", "moon-bytes");

  auto cached = sun::MoonCache::fetch(url, std::nullopt, fx.cacheDir);
  ASSERT_TRUE(fs::exists(cached));
  EXPECT_TRUE(cached.is_absolute());
  EXPECT_EQ(readFile(cached), "moon-bytes");

  // Source gone: the second fetch is served from the cache
  fs::remove_all(fx.srcDir);
  auto again = sun::MoonCache::fetch(url, std::nullopt, fx.cacheDir);
  EXPECT_EQ(again, cached);
  EXPECT_EQ(readFile(again), "moon-bytes");
}

TEST(Modules_MoonCache, fetch_preseeded_cache_skips_download_without_hash) {
  CacheFixture fx("preseeded");
  std::string url = "https://invalid.invalid/lib.moon";

  // Seed the cache entry the URL maps to; the network is never touched
  auto probe = fx.writeSource("probe.moon", "seeded");
  std::string fileUrl = probe;
  auto seeded = sun::MoonCache::fetch(fileUrl, std::nullopt, fx.cacheDir);
  // Rename the seeded entry to the name the https URL would get
  auto wanted = sha256Hex(url) + "-lib.moon";
  fs::rename(seeded, fx.cacheDir / wanted);

  auto cached = sun::MoonCache::fetch(url, std::nullopt, fx.cacheDir);
  EXPECT_EQ(readFile(cached), "seeded");
}

TEST(Modules_MoonCache, fetch_redownloads_on_hash_mismatch) {
  CacheFixture fx("redownload");
  std::string real = "real-content";
  std::string url = fx.writeSource("lib.moon", real);

  // Seed the cache with stale content under the URL's cache name
  fs::path stale = fx.cacheDir / (sha256Hex(url) + "-lib.moon");
  {
    std::ofstream out(stale, std::ios::binary);
    out << "stale-content";
  }

  auto cached = sun::MoonCache::fetch(url, sha256Hex(real), fx.cacheDir);
  EXPECT_EQ(readFile(cached), real);
}

TEST(Modules_MoonCache, fetch_errors_when_fresh_download_mismatches_hash) {
  CacheFixture fx("bad_hash");
  std::string url = fx.writeSource("lib.moon", "whatever");

  EXPECT_THROW(sun::MoonCache::fetch(url, std::string(64, 'f'), fx.cacheDir),
               SunError);
  // No cache entry (or temp file) left behind
  EXPECT_TRUE(fs::is_empty(fx.cacheDir));
}

TEST(Modules_MoonCache, github_token_is_sent_only_to_github_hosts) {
  sun::MoonCache::setGithubToken("ghp_abc123");

  auto github = sun::MoonCache::buildCurlCommand(
      "https://github.com/o/r/releases/download/v1/lib.moon", "out.moon");
  EXPECT_NE(github.find("Authorization: Bearer ghp_abc123"), std::string::npos);

  auto raw = sun::MoonCache::buildCurlCommand(
      "https://raw.githubusercontent.com/o/r/main/lib.moon", "out.moon");
  EXPECT_NE(raw.find("Authorization: Bearer"), std::string::npos);

  // The GitHub API asset endpoint needs the octet-stream Accept type
  auto api = sun::MoonCache::buildCurlCommand(
      "https://api.github.com/repos/o/r/releases/assets/123", "out.moon");
  EXPECT_NE(api.find("Authorization: Bearer"), std::string::npos);
  EXPECT_NE(api.find("Accept: application/octet-stream"), std::string::npos);
  EXPECT_EQ(github.find("Accept: application/octet-stream"), std::string::npos);

  // Never sent to other servers — including lookalike hosts
  auto other = sun::MoonCache::buildCurlCommand("https://example.com/lib.moon",
                                                "out.moon");
  EXPECT_EQ(other.find("Authorization"), std::string::npos);
  auto lookalike = sun::MoonCache::buildCurlCommand(
      "https://evilgithub.com/lib.moon", "out.moon");
  EXPECT_EQ(lookalike.find("Authorization"), std::string::npos);

  sun::MoonCache::setGithubToken("");
}

TEST(Modules_MoonCache, github_token_falls_back_to_environment) {
  sun::MoonCache::setGithubToken("");
  setenv("GH_TOKEN", "ghp_from_env", 1);

  auto command = sun::MoonCache::buildCurlCommand(
      "https://github.com/o/r/releases/download/v1/lib.moon", "out.moon");
  EXPECT_NE(command.find("Authorization: Bearer ghp_from_env"),
            std::string::npos);

  unsetenv("GH_TOKEN");
}

TEST(Modules_MoonCache, github_token_with_bad_characters_is_rejected) {
  sun::MoonCache::setGithubToken("bad'token");
  EXPECT_THROW(sun::MoonCache::buildCurlCommand(
                   "https://github.com/o/r/lib.moon", "out.moon"),
               SunError);
  sun::MoonCache::setGithubToken("");
}

TEST(Modules_MoonCache, fetch_rejects_malformed_url) {
  CacheFixture fx("malformed");
  EXPECT_THROW(
      sun::MoonCache::fetch("ftp://x/lib.moon", std::nullopt, fx.cacheDir),
      SunError);
  EXPECT_THROW(
      sun::MoonCache::fetch("https://x/lib'.moon", std::nullopt, fx.cacheDir),
      SunError);
  EXPECT_THROW(
      sun::MoonCache::fetch("https://x/a b.moon", std::nullopt, fx.cacheDir),
      SunError);
}
