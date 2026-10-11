/** Checks immutable metadata reuse without serving stale bundle contents. */
#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <unistd.h>

#include <filesystem>

#include "moon_bundling/moon.h"

/** Replacing a bundle at the same path and identity exposes its new metadata.
 */
TEST(Modules_MetadataCache, ReplacementDoesNotMutateExistingReaders) {
  using sun::moon_bundling::MoonReader;
  using sun::moon_bundling::MoonWriter;
  const auto path = std::filesystem::path("tmp") /
                    ("metadata_cache_" + std::to_string(getpid()) + ".moon");
  std::filesystem::create_directories(path.parent_path());
  llvm::LLVMContext context;
  llvm::Module module("cache", context);
  sun::moon::ModuleMetadata metadata;
  metadata.set_module_name("cache");
  const std::string hash(64, 'a');
  module.setTargetTriple("x86_64-unknown-linux-gnu");
  MoonWriter original(hash);
  original.addModule(module, metadata);
  ASSERT_TRUE(original.write(path));
  auto first = MoonReader::open(path);
  auto second = MoonReader::open(path);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(first->listModules().size(), 1u);
  const auto key = first->listModules().front();
  const auto* firstMetadata = first->getMetadata(key);
  ASSERT_NE(firstMetadata, nullptr);
  EXPECT_EQ(second->getMetadata(key), firstMetadata);

  module.setTargetTriple("aarch64-unknown-linux-gnu");
  MoonWriter replacement(hash);
  replacement.addModule(module, metadata);
  ASSERT_TRUE(replacement.write(path));
  auto third = MoonReader::open(path);
  ASSERT_NE(third, nullptr);
  const auto* replaced = third->getMetadata(key);
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(replaced->target_triple(), "aarch64-unknown-linux-gnu");
  EXPECT_EQ(firstMetadata->target_triple(), "x86_64-unknown-linux-gnu");
  std::filesystem::remove(path);
}
