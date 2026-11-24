/** Exercises persistent validation receipts and content-based invalidation. */
#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Host.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>

#include "abi/bundle_validation_cache.h"
#include "moon_bundling/moon.h"
#include "support/content_hash.h"
#include "support/error.h"

/** Isolates cache files and environment from unrelated compiler tests. */
namespace {
/** Shortens filesystem operations in isolated cache fixtures. */
namespace fs = std::filesystem;
using sun::abi::BundleValidationCache;

/** Owns one temporary cache and restores environment overrides after the test.
 */
class ABI_ValidationCache : public testing::Test {
 protected:
  fs::path root;
  std::optional<std::string> previousRoot, previousDisable;

  /** Creates an independent cache directory for each test process. */
  void SetUp() override {
    root = fs::current_path() / "tmp/validation-cache" /
           (std::string(
                testing::UnitTest::GetInstance()->current_test_info()->name()) +
            "-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    if (const auto* value = getenv("SUN_VALIDATION_CACHE")) {
      previousRoot = value;
    }
    if (const auto* value = getenv("SUN_DISABLE_VALIDATION_CACHE")) {
      previousDisable = value;
    }
    setenv("SUN_VALIDATION_CACHE", root.c_str(), 1);
    unsetenv("SUN_DISABLE_VALIDATION_CACHE");
    BundleValidationCache::clearMemory();
  }

  /** Restores process policy and releases temporary cache state. */
  void TearDown() override {
    if (previousRoot) {
      setenv("SUN_VALIDATION_CACHE", previousRoot->c_str(), 1);
    } else {
      unsetenv("SUN_VALIDATION_CACHE");
    }
    if (previousDisable) {
      setenv("SUN_DISABLE_VALIDATION_CACHE", previousDisable->c_str(), 1);
    } else {
      unsetenv("SUN_DISABLE_VALIDATION_CACHE");
    }
    BundleValidationCache::clearMemory();
    fs::remove_all(root);
  }

  /** Derives a real receipt identity using this executable and host runtime. */
  std::string key(const std::string& bytes = "bundle") {
    return BundleValidationCache::key(sun::support::blake3Hex(bytes), {}, "");
  }
};
}  // namespace

/** Checks official BLAKE3 vectors, including embedded zeroes and multiple
 * chunks.
 * https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json
 */
TEST(ABI_ContentHash, OfficialVectors) {
  for (const auto& [length, expected] :
       std::vector<std::pair<size_t, std::string>>{
           {0,
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
           {1,
            "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
           {2048,
            "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24"
            "a"}}) {
    std::string bytes(length, '\0');
    for (size_t i = 0; i < length; ++i) {
      bytes[i] = static_cast<char>(i % 251);
    }
    EXPECT_EQ(sun::support::blake3Hex(bytes), expected);
  }
}

/** External SHA-256 checksums retain their specified algorithm. */
TEST(ABI_ContentHash, ExternalChecksum) {
  EXPECT_EQ(sun::support::sha256Hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

/** Independent memory state can recover a success from disk. */
TEST_F(ABI_ValidationCache, PersistsSuccess) {
  auto identity = key();
  EXPECT_FALSE(BundleValidationCache::contains(identity));
  BundleValidationCache::remember(identity);
  BundleValidationCache::clearMemory();
  EXPECT_TRUE(BundleValidationCache::contains(identity));
  EXPECT_FALSE(BundleValidationCache::contains(key("changed")));
}

/** Invalid records never authorize a cache hit. */
TEST_F(ABI_ValidationCache, RejectsCorruptReceipts) {
  const auto identity = key();
  BundleValidationCache::remember(identity);
  const auto path = root / (identity + ".record");
  const auto size = fs::file_size(path);
  std::ofstream(path) << std::string(size, 'x');
  BundleValidationCache::clearMemory();
  EXPECT_FALSE(BundleValidationCache::contains(identity));
  std::ofstream(path) << "partial";
  EXPECT_FALSE(BundleValidationCache::contains(identity));
}

/** Force policy and the environment switch bypass both memory and disk hits. */
TEST_F(ABI_ValidationCache, ForceAndDisableBypassReceipts) {
  const auto identity = key();
  BundleValidationCache::remember(identity);
  {
    sun::abi::ValidationCacheScope force({}, true);
    EXPECT_FALSE(BundleValidationCache::contains(identity));
  }
  EXPECT_TRUE(BundleValidationCache::contains(identity));
  setenv("SUN_DISABLE_VALIDATION_CACHE", "1", 1);
  EXPECT_FALSE(BundleValidationCache::contains(identity));
}

/** Cache storage failure does not prevent successful compilation. */
TEST_F(ABI_ValidationCache, UnusableDirectoryIsNonfatal) {
  const auto identity = key();
  const auto file = root / "file";
  std::ofstream(file) << "not a directory";
  setenv("SUN_VALIDATION_CACHE", file.c_str(), 1);
  EXPECT_FALSE(BundleValidationCache::contains(identity));
  EXPECT_NO_THROW(BundleValidationCache::remember(identity));
  BundleValidationCache::clearMemory();
  EXPECT_FALSE(BundleValidationCache::contains(identity));
}

/** Compilation settings and explicit targets cannot share authorization. */
TEST_F(ABI_ValidationCache, SeparatesContexts) {
  sun::abi::CompilationContext context;
  const auto digest = sun::support::blake3Hex("bytes");
  const auto original =
      BundleValidationCache::key(digest, context, "x86_64-linux-gnu");
  context.exceptionTraceDepth++;
  EXPECT_NE(original,
            BundleValidationCache::key(digest, context, "x86_64-linux-gnu"));
  context.exceptionTraceDepth--;
  context.execution = sun::abi::NativeExecution::NativeLink;
  EXPECT_NE(original,
            BundleValidationCache::key(digest, context, "x86_64-linux-gnu"));
  context.execution = sun::abi::NativeExecution::JIT;
  EXPECT_NE(original,
            BundleValidationCache::key(digest, context, "aarch64-linux-gnu"));
  context.linkOptions.emplace_back("library", "changed");
  EXPECT_NE(original,
            BundleValidationCache::key(digest, context, "x86_64-linux-gnu"));
}

/** Concurrent processes publish complete interchangeable success records. */
TEST_F(ABI_ValidationCache, ConcurrentProcesses) {
  const auto identity = key();
  std::vector<pid_t> children;
  for (unsigned i = 0; i < 3; ++i) {
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (!child) {
      BundleValidationCache::clearMemory();
      BundleValidationCache::remember(identity);
      _exit(0);
    }
    children.push_back(child);
  }
  for (const auto child : children) {
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }
  BundleValidationCache::clearMemory();
  EXPECT_TRUE(BundleValidationCache::contains(identity));
}

/** Reopened bundles reuse validation while changed bytes get another identity.
 */
TEST_F(ABI_ValidationCache, CheckerPublishesContentReceipts) {
  llvm::LLVMContext llvmContext;
  llvm::Module module("cached", llvmContext);
  module.setTargetTriple(llvm::sys::getDefaultTargetTriple());
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("cached");
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  writer.addModule(module, metadata);
  const auto path = root / "bundle.moon";
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker first;
  ASSERT_NO_THROW(first.validate(*reader));
  const auto identity =
      BundleValidationCache::key(first.bytesHash(*reader), first.context(), "");
  BundleValidationCache::clearMemory();
  EXPECT_TRUE(BundleValidationCache::contains(identity));
  sun::abi::AbiCompatibilityChecker second;
  auto reopened = sun::moon_bundling::MoonReader::open(path);
  EXPECT_NO_THROW(second.validate(*reopened));
  EXPECT_EQ(second.importedContracts().size(), 1u);
  const auto timestamp = fs::last_write_time(path);
  auto bytes = *reader->getBytes();
  bytes.back() ^= 1;
  std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
  fs::last_write_time(path, timestamp);
  EXPECT_FALSE(BundleValidationCache::contains(BundleValidationCache::key(
      sun::support::blake3Hex(bytes), second.context(), "")));
}

/** A valid record copied under another bundle's key cannot authorize it. */
TEST_F(ABI_ValidationCache, RejectsMismatchedReceipt) {
  const auto original = key();
  const auto changed = key("changed");
  BundleValidationCache::remember(original);
  fs::copy_file(root / (original + ".record"), root / (changed + ".record"));
  BundleValidationCache::clearMemory();
  EXPECT_FALSE(BundleValidationCache::contains(changed));
}
