/** Exercises conservative reconstruction of native archive requirements. */
#include <gtest/gtest.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>

#include "abi/native_requirements.h"
#include "moon_bundling/archive_symbols.h"
#include "support/error.h"

/** Provides an actual compiled native fixture without depending on runtime lookup. */
namespace {
/** Reads and isolates the native fixture just as Moon packaging does. */
std::string nativeFixture(bool isolate, bool secondVersion = false) {
  const auto directory = secondVersion ? SUN_FFI_TESTLIB_V2_DIR : SUN_FFI_TESTLIB_DIR;
  auto bytes = llvm::MemoryBuffer::getFile(std::string(directory) + "/libsun_ffi_static_testlib.a");
  if (!bytes) throw std::runtime_error("Native fixture is missing");
  if (!isolate) return (*bytes)->getBuffer().str();
  auto scan = sun::moon_bundling::scanArchiveSymbols((*bytes)->getMemBufferRef());
  if (!scan) throw std::runtime_error(llvm::toString(scan.takeError()));
  std::map<std::string, std::string> renames;
  for (const auto& symbol : scan->defined) renames.emplace(symbol, "$test$_" + symbol);
  auto renamed = sun::moon_bundling::renameArchiveSymbols((*bytes)->getMemBufferRef(), renames);
  if (!renamed) throw std::runtime_error(llvm::toString(renamed.takeError()));
  return std::move(*renamed);
}
}

/** Symbol isolation is necessary but does not authorize an unsupported target. */
TEST(ABI_Compatibility, NativeProfileChecksPlatformAndSymbolNamespace) {
  auto bytes = nativeFixture(true);
  std::vector<sun::abi::NativeArchiveInput> input{
      {llvm::MemoryBufferRef(bytes, "fixture.a"), "$test$_"}};
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(input,
      sun::abi::compatibilityTarget("x86_64-linux-gnu")));
  EXPECT_THROW(sun::abi::validateNativeArchives(input,
      sun::abi::compatibilityTarget("aarch64-linux-gnu")), sun::support::SunError);
  bytes = nativeFixture(false);
  input = {{llvm::MemoryBufferRef(bytes, "fixture.a"), "$test$_"}};
  EXPECT_THROW(sun::abi::validateNativeArchives(input,
      sun::abi::compatibilityTarget("x86_64-linux-gnu")), sun::support::SunError);
}

/** An unrecorded runtime entry instruction is rejected even in an isolated archive. */
TEST(ABI_Compatibility, NativeInstructionsAreIndependentlyDecoded) {
  auto bytes = nativeFixture(true);
  auto archive = llvm::object::Archive::create(llvm::MemoryBufferRef(bytes, "fixture.a"));
  ASSERT_TRUE(static_cast<bool>(archive)) << llvm::toString(archive.takeError());
  auto error = llvm::Error::success();
  bool changed = false;
  for (const auto& child : (*archive)->children(error)) {
    auto binary = child.getAsBinary();
    ASSERT_TRUE(static_cast<bool>(binary)) << llvm::toString(binary.takeError());
    const auto* object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->get());
    ASSERT_NE(object, nullptr);
    for (const auto& section : object->sections()) {
      if (!section.isText() || section.getSize() < 2) continue;
      auto code = section.getContents();
      ASSERT_TRUE(static_cast<bool>(code)) << llvm::toString(code.takeError());
      const auto offset = bytes.find(code->str());
      ASSERT_NE(offset, std::string::npos);
      bytes[offset] = '\x0f';
      bytes[offset + 1] = '\x05';  // SYSCALL needs an explicitly supported runtime contract.
      changed = true;
      break;
    }
    if (changed) break;
  }
  ASSERT_FALSE(static_cast<bool>(error)) << llvm::toString(std::move(error));
  ASSERT_TRUE(changed);
  const std::vector<sun::abi::NativeArchiveInput> input{
      {llvm::MemoryBufferRef(bytes, "fixture.a"), "$test$_"}};
  EXPECT_THROW(sun::abi::validateNativeArchives(input,
      sun::abi::compatibilityTarget("x86_64-linux-gnu")), sun::support::SunError);
}

/** Runtime services are recorded only when the native payload actually references them. */
TEST(ABI_Compatibility, NativeRuntimeRequirementsFollowActualReferences) {
  auto first = nativeFixture(true);
  auto second = nativeFixture(true, true);
  const auto profile = sun::abi::compatibilityTarget("x86_64-linux-gnu");
  std::set<std::string> services;
  sun::abi::validateNativeArchives({{llvm::MemoryBufferRef(first, "first.a"), "$test$_"}},
                                  profile, &services);
  EXPECT_TRUE(services.empty());
  sun::abi::validateNativeArchives({{llvm::MemoryBufferRef(second, "second.a"), "$test$_"}},
                                  profile, &services);
  EXPECT_EQ(services, std::set<std::string>{"atexit"});
}
