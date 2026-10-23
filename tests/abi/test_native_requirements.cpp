/** Exercises native archive structure and declared requirement validation. */
#include <gtest/gtest.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/BinaryFormat/MachO.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>

#include "abi/abi_compatibility_checker.h"
#include "abi/native_requirements.h"
#include "driver/compiler.h"
#include "driver/input_hash.h"
#include "driver/manifest_processor.h"
#include "moon_bundling/archive_symbols.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "parsing/parser.h"
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
    renames.emplace(symbol, "$abcd$_" + symbol);
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
      {llvm::MemoryBufferRef(bytes, "fixture.a"), "$abcd$_"}};
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      input,
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple())));
  EXPECT_THROW(
      sun::abi::validateNativeArchives(
          input,
          sun::abi::compatibilityTarget(
              sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple())
                          .architecture() == "aarch64"
                  ? "x86_64-linux-gnu"
                  : "aarch64-linux-gnu")),
      sun::support::SunError);
  bytes = nativeFixture(false);
  input = {{llvm::MemoryBufferRef(bytes, "fixture.a"), "$abcd$_"}};
  EXPECT_THROW(sun::abi::validateNativeArchives(
                   input, sun::abi::compatibilityTarget(
                              llvm::sys::getDefaultTargetTriple())),
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
        {llvm::MemoryBufferRef(bytes, "fixture.a"), "$abcd$_"}};
    EXPECT_NO_THROW(sun::abi::validateNativeArchives(
        input,
        sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple())));
  }
}

/** Runtime services are recorded only when the native payload actually
 * references them. */
TEST(ABI_Compatibility, NativeRuntimeRequirementsFollowActualReferences) {
  auto first = nativeFixture(true);
  auto second = nativeFixture(true, true);
  const auto profile =
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple());
  std::set<std::string> services;
  sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(first, "first.a"), "$abcd$_"}}, profile,
      &services);
  EXPECT_TRUE(services.empty());
  sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(second, "second.a"), "$abcd$_"}}, profile,
      &services);
  EXPECT_EQ(services, (std::set<std::string>{"atexit", "linker"}));
}

/** Trusting native instructions does not authorize malformed archive
 * containers. */
TEST(ABI_Compatibility, NativeArchiveStructureStillRequiresValidation) {
  const std::string invalid = "not an archive";
  EXPECT_THROW(
      sun::abi::validateNativeArchives(
          {{llvm::MemoryBufferRef(invalid, "invalid.a"), "$abcd$_"}},
          sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple())),
      sun::support::SunError);
}

/** External references are declared for resolution by the actual selected
 * linker. */
TEST(ABI_Compatibility, NativeExternalReferencesAreRecordedForTheLinker) {
  const auto bytes = nativeFixture(true, true);
  auto renamed = sun::moon_bundling::renameArchiveSymbols(
      llvm::MemoryBufferRef(bytes, "fixture.a"),
      {{"atexit", "sun_test_unknown_runtime"}});
  ASSERT_TRUE(static_cast<bool>(renamed))
      << llvm::toString(renamed.takeError());
  std::set<std::string> services, symbols;
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(*renamed, "fixture.a"), "$abcd$_"}},
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple()),
      &services, &symbols));
  EXPECT_EQ(services, std::set<std::string>{"linker"});
  EXPECT_EQ(symbols, std::set<std::string>{"sun_test_unknown_runtime"});
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
        if (object->isMachO())
          foundGotReference |=
              object->getArch() == llvm::Triple::aarch64
                  ? type == llvm::MachO::ARM64_RELOC_GOT_LOAD_PAGE21 ||
                        type == llvm::MachO::ARM64_RELOC_GOT_LOAD_PAGEOFF12
                  : type == llvm::MachO::X86_64_RELOC_GOT_LOAD ||
                        type == llvm::MachO::X86_64_RELOC_GOT;
        else if (object->getArch() == llvm::Triple::aarch64)
          foundGotReference |= type == llvm::ELF::R_AARCH64_ADR_GOT_PAGE ||
                               type == llvm::ELF::R_AARCH64_LD64_GOT_LO12_NC;
        else
          foundGotReference |= type == llvm::ELF::R_X86_64_GOTPCREL ||
                               type == llvm::ELF::R_X86_64_GOTPCRELX ||
                               type == llvm::ELF::R_X86_64_REX_GOTPCRELX;
      }
  }
  ASSERT_FALSE(static_cast<bool>(error)) << llvm::toString(std::move(error));
  ASSERT_TRUE(foundGotReference);
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(bytes, "relocations.a"), "$abcd$_"}},
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple())));
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
      {{llvm::MemoryBufferRef(*renamed, "fixture.a"), "$abcd$_"}},
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple()),
      &services));
  EXPECT_TRUE(services.empty());
}

/** ELF object structure can be checked for a declared musl target without host
 * libc inference. */
TEST(ABI_Compatibility, NativeELFSupportsDeclaredMuslTarget) {
  if (!llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSLinux())
    GTEST_SKIP() << "This fixture tests ELF libc declarations";
  const auto bytes = nativeFixture(true);
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(bytes, "fixture.a"), "$abcd$_"}},
      sun::abi::compatibilityTarget(
          sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple())
              .architecture() +
          "-linux-musl")));
  EXPECT_EQ(sun::abi::nativeArchiveRule(
                sun::abi::compatibilityTarget("aarch64-linux-musl")),
            "native.elf.aarch64.declared");
}

/** Native symbol diagnostics do not gate imports or replace linker resolution.
 */
TEST(ABI_Compatibility, NativeSymbolDiagnosticsDoNotGateImport) {
  llvm::LLVMContext context;
  llvm::Module module("native", context);
  module.setTargetTriple(llvm::sys::getDefaultTargetTriple());
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("native");
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  writer.addModule(module, metadata);
  writer.addNativeArchive("abcd", "fixture.a", nativeFixture(true, true));
  std::filesystem::create_directories("tmp");
  const auto path = std::filesystem::path("tmp") /
                    ("native_symbols_" + std::to_string(getpid()) + ".moon");
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker(
      llvm::sys::getDefaultTargetTriple());
  EXPECT_NO_THROW(checker.validate(*reader));
  auto altered = reader->getLibrary();
  auto* contract = altered.mutable_bundle_contract();
  ASSERT_EQ(contract->native_link_symbols_size(), 1);
  EXPECT_EQ(contract->native_link_symbols(0), "atexit");
  contract->set_native_link_symbols(0, "mtexit");
  contract->set_compatibility_hash(sun::abi::hashBundleContract(*contract));
  EXPECT_NO_THROW(checker.validateContract(
      *contract, "native.moon", llvm::sys::getDefaultTargetTriple()));
  const auto before = reader->getLibrary().SerializeAsString();
  const auto after = altered.SerializeAsString();
  ASSERT_EQ(before.size(), after.size());
  auto bytes = *reader->getBytes();
  const auto offset = bytes.find(before);
  ASSERT_NE(offset, std::string::npos);
  bytes.replace(offset, before.size(), after);
  {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
  }
  auto changed = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(changed, nullptr);
  EXPECT_NO_THROW(checker.validate(*changed));
  std::filesystem::remove(path);
}

/** Reads native state initialized by the executable runtime. */
int sun_abi_read_lifecycle_value();

/** Native initialization and TLS are recorded and authorized only for native
 * linking. */
TEST(ABI_Compatibility, NativeLifecycleRequirementsDependOnExecutionMode) {
  EXPECT_EQ(sun_abi_read_lifecycle_value(), 42);
  const auto bytes = nativeFixture(true, false, "libsun_abi_lifecycle.a");
  std::set<std::string> services;
  sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(bytes, "lifecycle.a"), "$abcd$_"}},
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple()),
      &services);
  EXPECT_TRUE(services.count("initializers"));
  EXPECT_TRUE(services.count("thread_local"));
  llvm::LLVMContext context;
  llvm::Module module("lifecycle", context);
  module.setTargetTriple(llvm::sys::getDefaultTargetTriple());
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("lifecycle");
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  writer.addModule(module, metadata);
  writer.addNativeArchive("abcd", "lifecycle.a", bytes);
  std::filesystem::create_directories("tmp");
  const auto path = std::filesystem::path("tmp") /
                    ("native_lifecycle_" + std::to_string(getpid()) + ".moon");
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker(
      llvm::sys::getDefaultTargetTriple(),
      sun::abi::NativeExecution::NativeLink);
  EXPECT_NO_THROW(checker.validate(*reader));
  sun::driver::BuildInputs inputs;
  inputs.artifactKind = "executable";
  inputs.targetTriple = llvm::sys::getDefaultTargetTriple();
  inputs.moonImports.emplace_back(path.string());
  EXPECT_NO_THROW(sun::driver::computeInputHash(inputs));
  checker.beginCompilation(llvm::sys::getDefaultTargetTriple(),
                           sun::abi::NativeExecution::JIT);
  EXPECT_TRUE(checker.importedContracts().empty());
  EXPECT_THROW(checker.validate(*reader), sun::support::SunError);
  EXPECT_TRUE(checker.importedContracts().empty());
  checker.beginCompilation(llvm::sys::getDefaultTargetTriple(),
                           sun::abi::NativeExecution::NativeLink);
  EXPECT_NO_THROW(checker.validate(*reader));
  std::filesystem::remove(path);
}

/** Declaring an external reference does not suppress an actual native link
 * failure. */
TEST(ABI_Compatibility, MissingNativeRuntimeSymbolFailsAtLinkTime) {
  const auto bytes = nativeFixture(true, true);
  auto renamed = sun::moon_bundling::renameArchiveSymbols(
      llvm::MemoryBufferRef(bytes, "fixture.a"),
      {{"atexit", "sun_test_missing_native_runtime"}});
  ASSERT_TRUE(static_cast<bool>(renamed))
      << llvm::toString(renamed.takeError());
  std::set<std::string> services, symbols;
  EXPECT_NO_THROW(sun::abi::validateNativeArchives(
      {{llvm::MemoryBufferRef(*renamed, "fixture.a"), "$abcd$_"}},
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple()),
      &services, &symbols));
  EXPECT_TRUE(symbols.count("sun_test_missing_native_runtime"));
  const auto dir = std::filesystem::path("tmp") /
                   ("native_missing_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  const auto archive = dir / "missing.a";
  {
    std::ofstream output(archive, std::ios::binary);
    output.write(renamed->data(), renamed->size());
  }
  llvm::LLVMContext context;
  llvm::Module module("missing", context);
  module.setTargetTriple(llvm::sys::getDefaultTargetTriple());
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* setter = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {llvm::Type::getInt32Ty(context)}, false),
      llvm::Function::ExternalLinkage, "$abcd$_sun_ffi_slot_set", module);
  auto* main = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context), false),
      llvm::Function::ExternalLinkage, "main", module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", main));
  builder.CreateCall(setter, {builder.getInt32(42)});
  builder.CreateRet(builder.getInt32(0));
  sun::driver::LinkOptions options;
  options.archives.push_back(archive.string());
  std::string error;
  EXPECT_FALSE(sun::driver::compileToExecutable(
      module, (dir / "program").string(), error, false, options));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(std::filesystem::exists(dir / "program"));
  std::filesystem::remove_all(dir);
}

/** Explicit archive requirements survive packaging and govern import behavior.
 */
TEST(ABI_Compatibility, ManifestNativeRequirementsReachTheBundleContract) {
  const auto directory =
      std::filesystem::path("tmp") /
      ("abi_native_declarations_" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto source = directory / "library.sun";
  const auto output = directory / "library.moon";
  const std::string feature =
      sun::abi::compatibilityTarget(llvm::sys::getDefaultTargetTriple())
                  .architecture() == "aarch64"
          ? "neon"
          : "sse2";
  /** Writes one archive declaration without changing its native object bytes.
   */
  const auto writeSource = [&](const std::string& runtime) {
    std::ofstream file(source);
    file << "manifest { archives: [{ path: \"" << SUN_FFI_TESTLIB_DIR
         << "/libsun_abi_relocations.a\", "
         << "cpu_features: [\"" << feature
         << "\"], runtime: [" << runtime
         << "] }] }\n"
         << "/** Exports a scalar function. */\npublic module native_decl {\n"
         << "/** Returns the fixture value. */\n"
         << "public function value() i32 { return 42; } }\n";
  };
  writeSource("\"thread_local\"");
  sun::moon_bundling::MoonBuildOptions options;
  options.targetTriple = llvm::sys::getDefaultTargetTriple();
  ASSERT_NO_THROW(
      sun::moon_bundling::MoonBuilder::build(source.string(), output, options));
  auto reader = sun::moon_bundling::MoonReader::open(output);
  ASSERT_NE(reader, nullptr);
  const auto originalHash =
      reader->getLibrary().bundle_contract().compatibility_hash();
  const auto& features =
      reader->getLibrary().bundle_contract().target().cpu_features();
  EXPECT_NE(std::find(features.begin(), features.end(), feature),
            features.end());
  sun::abi::AbiCompatibilityChecker checker(
      options.targetTriple, sun::abi::NativeExecution::NativeLink);
  EXPECT_NO_THROW(checker.validate(*reader));
  checker.beginCompilation(options.targetTriple,
                           sun::abi::NativeExecution::JIT);
  EXPECT_THROW(checker.validate(*reader), sun::support::SunError);
  writeSource("");
  const auto report =
      sun::moon_bundling::MoonBuilder::build(source.string(), output, options);
  EXPECT_FALSE(report.upToDate);
  auto revised = sun::moon_bundling::MoonReader::open(output);
  ASSERT_NE(revised, nullptr);
  EXPECT_NE(originalHash,
            revised->getLibrary().bundle_contract().compatibility_hash());
  EXPECT_NO_THROW(checker.validate(*revised));
  std::filesystem::remove_all(directory);
}

/** Invalid producer declarations cannot partially modify an existing contract.
 */
TEST(ABI_Compatibility, UnknownNativeDeclarationsFailAtomically) {
  llvm::LLVMContext context;
  llvm::Module module("native", context);
  module.setTargetTriple("x86_64-linux-gnu");
  auto contract = sun::abi::describeContract(module);
  const auto original = contract.SerializeAsString();
  EXPECT_THROW(
      sun::abi::addNativeRequirements(contract, {"sse2"}, {"unknown-runtime"}),
      sun::support::SunError);
  EXPECT_EQ(original, contract.SerializeAsString());
  EXPECT_THROW(sun::abi::addNativeRequirements(contract, {"unknown-cpu"}, {}),
               sun::support::SunError);
  EXPECT_EQ(original, contract.SerializeAsString());
}

/** Malformed archive declarations fail before paths are resolved or code runs.
 */
TEST(ABI_Compatibility, MalformedArchiveDeclarationsAreRejected) {
  for (const auto* declaration : {"{ cpu_features: [\"avx2\"] }",
                                  "{ path: \"lib.a\", path: \"other.a\" }",
                                  "{ path: \"lib.a\", cpu_features: \"avx2\" }",
                                  "{ path: \"lib.a\", provenance: \"vendor build\" }",
                                  "{ path: \"lib.a\", unknown: [] }"}) {
    auto parser = sun::parsing::Parser::createStringParser(
        std::string("manifest { archives: [") + declaration + "] }");
    EXPECT_THROW(parser.parseProgram(), sun::support::SunError);
  }
}

/** Requirements from inactive target blocks must not leak into a bundle. */
TEST(ABI_Compatibility, NativeDeclarationsFollowManifestTargetSelection) {
  auto parser = sun::parsing::Parser::createStringParser(R"(
    manifest { target: {
      linux: { archives: [{ path: "linux.a", cpu_features: ["sse2"] }] },
      macos: { archives: [{ path: "mac.a", runtime: ["thread_local"] }] }
    } }
  )");
  auto program = parser.parseProgram();
  const auto* manifest = sun::driver::ManifestProcessor::findManifest(*program);
  ASSERT_NE(manifest, nullptr);
  const auto resolved = sun::driver::ManifestProcessor::process(
      *manifest, ".", "x86_64-linux-gnu");
  EXPECT_EQ(resolved.nativeCPUFeatures, std::vector<std::string>{"sse2"});
  EXPECT_TRUE(resolved.nativeRuntimeServices.empty());
  ASSERT_EQ(resolved.archiveFiles.size(), 1u);
  EXPECT_EQ(std::filesystem::path(resolved.archiveFiles[0]).filename(),
            "linux.a");
}

/** Both Mach-O architectures use declared trust while retaining structural,
 * namespace, target, and native lifecycle checks on every compiler host. */
TEST(ABI_Compatibility, MachOArchivesUseDeclaredTrust) {
  for (const auto* architecture : {"x86_64", "aarch64"}) {
    const auto archive = std::string("libsun_abi_") + architecture + "_macho.a";
    const auto profile = sun::abi::compatibilityTarget(
        std::string(architecture) + "-apple-macos11");
    EXPECT_EQ(sun::abi::nativeArchiveRule(profile),
              std::string("native.macho.") + architecture + ".declared");
    auto bytes = nativeFixture(true, false, archive.c_str());
    std::set<std::string> services;
    const std::vector<sun::abi::NativeArchiveInput> input{
        {llvm::MemoryBufferRef(bytes, "fixture.a"), "$abcd$_"}};
    EXPECT_NO_THROW(
        sun::abi::validateNativeArchives(input, profile, &services));
    EXPECT_TRUE(services.count("initializers"));
    EXPECT_TRUE(services.count("thread_local"));
    EXPECT_THROW(sun::abi::validateNativeArchives(
                     input, sun::abi::compatibilityTarget(
                                std::string(architecture) + "-linux-gnu")),
                 sun::support::SunError);
    EXPECT_THROW(
        sun::abi::validateNativeArchives(
            input,
            sun::abi::compatibilityTarget(
                std::string(architecture == std::string("x86_64") ? "aarch64"
                                                                  : "x86_64") +
                "-apple-macos11")),
        sun::support::SunError);
    const auto unisolated = nativeFixture(false, false, archive.c_str());
    EXPECT_THROW(
        sun::abi::validateNativeArchives(
            {{llvm::MemoryBufferRef(unisolated, "fixture.a"), "$abcd$_"}},
            profile),
        sun::support::SunError);
  }
}

/** Apple arm64 and LLVM aarch64 spelling select the same machine profile. */
TEST(ABI_Compatibility, AppleArchitectureAliasesShareAProfile) {
  EXPECT_EQ(
      sun::abi::compatibilityTarget("arm64-apple-macos11").SerializeAsString(),
      sun::abi::compatibilityTarget("aarch64-apple-macos11")
          .SerializeAsString());
}
