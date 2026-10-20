/** Exercises native archive structure and declared requirement validation. */
#include <gtest/gtest.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>

#include <algorithm>

#include "abi/native_requirements.h"
#include "moon_bundling/archive_symbols.h"
#include "support/error.h"

/** Reads the linked PIC fixture to verify the linker resolved its GOT
 * reference. */
int sun_abi_read_relocation_value();

/** Provides an actual compiled native fixture without depending on runtime
 * lookup. */
namespace {
/** Reads and isolates the native fixture just as Moon packaging does. */
std::string nativeFixture(
    bool isolate, bool secondVersion = false,
    const char* archiveName = "libsun_ffi_static_testlib.a") {
  const auto directory =
      secondVersion ? SUN_FFI_TESTLIB_V2_DIR : SUN_FFI_TESTLIB_DIR;
  auto bytes = llvm::MemoryBuffer::getFile(std::string(directory) +
                                           std::string("/") + archiveName);
  if (!bytes) throw std::runtime_error("Native fixture is missing");
  if (!isolate) return (*bytes)->getBuffer().str();
  auto scan =
      sun::moon_bundling::scanArchiveSymbols((*bytes)->getMemBufferRef());
  if (!scan) throw std::runtime_error(llvm::toString(scan.takeError()));
  std::map<std::string, std::string> renames;
  for (const auto& symbol : scan->defined)
    renames.emplace(symbol, "$test$_" + symbol);
  auto renamed = sun::moon_bundling::renameArchiveSymbols(
      (*bytes)->getMemBufferRef(), renames);
  if (!renamed) throw std::runtime_error(llvm::toString(renamed.takeError()));
  return std::move(*renamed);
}
}  // namespace

/** Symbol isolation is necessary but does not authorize an unsupported target.
 */
TEST(ABI_Compatibility, NativeProfileChecksPlatformAndSymbolNamespace) {
  auto bytes = nativeFixture(true);
  std::vector<sun::abi::NativeArchiveInput> input{
      {llvm::MemoryBufferRef(bytes, "fixture.a"), "$test$_"}};
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      input, sun::abi::compatibilityTarget("x86_64-linux-gnu")));
  EXPECT_THROW(sun::abi::validateNativeArchives(
                   input, sun::abi::compatibilityTarget("aarch64-linux-gnu")),
               sun::support::SunError);
  bytes = nativeFixture(false);
  input = {{llvm::MemoryBufferRef(bytes, "fixture.a"), "$test$_"}};
  EXPECT_THROW(sun::abi::validateNativeArchives(
                   input, sun::abi::compatibilityTarget("x86_64-linux-gnu")),
               sun::support::SunError);
}

/** Native instructions follow linker trust, including indirect calls and SIMD.
 */
TEST(ABI_Compatibility, NativeInstructionsDoNotRequireAnAllowlist) {
  // These bytes are inspected as archive contents, never executed.
  for (const std::string instruction :
       {std::string("\xff\x10", 2), std::string("\x0f\x05", 2),
        std::string("\xc5\xf4\x58\xd0", 4)}) {
    auto bytes = nativeFixture(true);
    auto archive = llvm::object::Archive::create(
        llvm::MemoryBufferRef(bytes, "fixture.a"));
    ASSERT_TRUE(static_cast<bool>(archive))
        << llvm::toString(archive.takeError());
    auto error = llvm::Error::success();
    bool changed = false;
    for (const auto& child : (*archive)->children(error)) {
      auto binary = child.getAsBinary();
      ASSERT_TRUE(static_cast<bool>(binary))
          << llvm::toString(binary.takeError());
      const auto* object =
          llvm::dyn_cast<llvm::object::ObjectFile>(binary->get());
      ASSERT_NE(object, nullptr);
      for (const auto& section : object->sections()) {
        if (!section.isText() || section.getSize() < instruction.size())
          continue;
        auto code = section.getContents();
        ASSERT_TRUE(static_cast<bool>(code))
            << llvm::toString(code.takeError());
        const auto offset = code->data() - bytes.data();
        ASSERT_GE(offset, 0);
        ASSERT_LE(static_cast<size_t>(offset) + instruction.size(),
                  bytes.size());
        std::copy(instruction.begin(), instruction.end(),
                  bytes.begin() + offset);
        changed = true;
        break;
      }
      if (changed) break;
    }
    ASSERT_FALSE(static_cast<bool>(error)) << llvm::toString(std::move(error));
    ASSERT_TRUE(changed);
    const std::vector<sun::abi::NativeArchiveInput> input{
        {llvm::MemoryBufferRef(bytes, "fixture.a"), "$test$_"}};
    EXPECT_NO_THROW(sun::abi::validateNativeArchives(
        input, sun::abi::compatibilityTarget("x86_64-linux-gnu")));
  }
}

/** Runtime services are recorded only when the native payload actually
 * references them. */
TEST(ABI_Compatibility, NativeRuntimeRequirementsFollowActualReferences) {
  auto first = nativeFixture(true);
  auto second = nativeFixture(true, true);
  const auto profile = sun::abi::compatibilityTarget("x86_64-linux-gnu");
  std::set<std::string> services;
  sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(first, "first.a"), "$test$_"}}, profile,
      &services);
  EXPECT_TRUE(services.empty());
  sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(second, "second.a"), "$test$_"}}, profile,
      &services);
  EXPECT_EQ(services, std::set<std::string>{"atexit"});
}

/** Trusting native instructions does not authorize malformed archive
 * containers. */
TEST(ABI_Compatibility, NativeArchiveStructureStillRequiresValidation) {
  const std::string invalid = "not an archive";
  EXPECT_THROW(sun::abi::validateNativeArchives(
                   {{llvm::MemoryBufferRef(invalid, "invalid.a"), "$test$_"}},
                   sun::abi::compatibilityTarget("x86_64-linux-gnu")),
               sun::support::SunError);
}

/** External runtime references still need an implemented compatibility rule. */
TEST(ABI_Compatibility, NativeUnknownRuntimeReferencesRemainUnsupported) {
  const auto bytes = nativeFixture(true, true);
  auto renamed = sun::moon_bundling::renameArchiveSymbols(
      llvm::MemoryBufferRef(bytes, "fixture.a"),
      {{"atexit", "sun_test_unknown_runtime"}});
  ASSERT_TRUE(static_cast<bool>(renamed))
      << llvm::toString(renamed.takeError());
  EXPECT_THROW(sun::abi::validateNativeArchives(
                   {{llvm::MemoryBufferRef(*renamed, "fixture.a"), "$test$_"}},
                   sun::abi::compatibilityTarget("x86_64-linux-gnu")),
               sun::support::SunError);
}

/** GOT references are accepted by ABI validation and left to the linker. */
TEST(ABI_Compatibility, NativeRelocationsAreDelegatedToTheLinker) {
  EXPECT_EQ(sun_abi_read_relocation_value(), 42);
  const auto bytes = nativeFixture(true, false, "libsun_abi_relocations.a");
  auto archive = llvm::object::Archive::create(
      llvm::MemoryBufferRef(bytes, "relocations.a"));
  ASSERT_TRUE(static_cast<bool>(archive))
      << llvm::toString(archive.takeError());
  auto error = llvm::Error::success();
  bool foundGotReference = false;
  for (const auto& child : (*archive)->children(error)) {
    auto binary = child.getAsBinary();
    ASSERT_TRUE(static_cast<bool>(binary))
        << llvm::toString(binary.takeError());
    const auto* object =
        llvm::dyn_cast<llvm::object::ObjectFile>(binary->get());
    ASSERT_NE(object, nullptr);
    for (const auto& section : object->sections())
      for (const auto& relocation : section.relocations()) {
        const auto type = relocation.getType();
        foundGotReference |= type == llvm::ELF::R_X86_64_GOTPCREL ||
                             type == llvm::ELF::R_X86_64_GOTPCRELX ||
                             type == llvm::ELF::R_X86_64_REX_GOTPCRELX;
      }
  }
  ASSERT_FALSE(static_cast<bool>(error)) << llvm::toString(std::move(error));
  ASSERT_TRUE(foundGotReference);
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(bytes, "relocations.a"), "$test$_"}},
      sun::abi::compatibilityTarget("x86_64-linux-gnu")));
}

/** The ELF linker's GOT symbol does not require a native runtime service. */
TEST(ABI_Compatibility, NativeGotBaseIsSuppliedByTheLinker) {
  const auto bytes = nativeFixture(true, true);
  auto renamed = sun::moon_bundling::renameArchiveSymbols(
      llvm::MemoryBufferRef(bytes, "fixture.a"),
      {{"atexit", "_GLOBAL_OFFSET_TABLE_"}});
  ASSERT_TRUE(static_cast<bool>(renamed))
      << llvm::toString(renamed.takeError());
  std::set<std::string> services;
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(*renamed, "fixture.a"), "$test$_"}},
      sun::abi::compatibilityTarget("x86_64-linux-gnu"), &services));
  EXPECT_TRUE(services.empty());
}
