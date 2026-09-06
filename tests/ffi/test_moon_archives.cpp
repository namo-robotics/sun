// tests/ffi/test_moon_archives.cpp — Native archives carried across bundles
//
// A bundle wrapping a C library carries that library's `.a` (manifest
// `archives:`). A second bundle built on the first inlines its bitcode, calls
// included, so it must carry the archive as well: a program importing only
// the second bundle never sees the first one, yet has to link the C code
// (issue #218). The archive's symbols are renamed under a hash of the
// bundle's archives when the bundle is built, so two bundles carrying
// different versions of one library link into one program, each bound to
// its own copy, while two bundles carrying the same bytes share one. The
// fixtures are two builds of libsun_ffi_static_testlib.a, whose symbols are
// absent from the test binary and have no shared-library counterpart, so
// neither the AOT link nor the JIT can satisfy them by accident.

#include <gtest/gtest.h>

#include <llvm/Support/MemoryBufferRef.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/compiler.h"
#include "driver/execution_utils.h"
#include "moon_bundling/archive_symbols.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "moon_bundling/moon_import.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kArchiveName = "libsun_ffi_static_testlib.a";

// Directories holding the two built versions of the test archive, baked in
// by CMake. Empty when the define is absent (e.g. an ad-hoc build).
std::string ffiTestLibDir() {
#ifdef SUN_FFI_TESTLIB_DIR
  return SUN_FFI_TESTLIB_DIR;
#else
  return {};
#endif
}

std::string ffiTestLibV2Dir() {
#ifdef SUN_FFI_TESTLIB_V2_DIR
  return SUN_FFI_TESTLIB_V2_DIR;
#else
  return {};
#endif
}

// A module wrapping the two C entry points of the fixture archive
std::string wrapperSource(const std::string& moduleName,
                          const std::string& archive) {
  return "public module " + moduleName + R"( {
  extern "C" function sun_ffi_slot_set(v: i32) void;
  extern "C" function sun_ffi_slot_get() i32;

  public function store(v: i32) void {
    unsafe { sun_ffi_slot_set(v); };
  }

  public function load() i32 {
    return unsafe { sun_ffi_slot_get(); };
  }
}

manifest {
  archives: [")" + archive + R"("]
}
)";
}

// Four bundles in a scratch directory: `leaf.moon` wraps the C archive and
// carries it; `mid.moon` is built on leaf and names no archive itself;
// `twin.moon` wraps the same archive file independently of leaf;
// `leaf2.moon` wraps the second version of the archive. Built once per
// process (see chain()): the library cache is a singleton that keeps every
// bundle it has opened, so rebuilding under a fresh path per test would
// leave it holding readers for files that no longer exist. The directory is
// per process too: ctest runs each test as its own process, several at a
// time, and they must not wipe each other's bundles.
struct BundleChain {
  fs::path dir;
  fs::path leaf;
  fs::path mid;
  fs::path twin;
  fs::path leaf2;
  sun::MoonBuildReport leafReport;
  sun::MoonBuildReport midReport;

  BundleChain() {
    initTestEnvironment();
    dir = fs::path(::testing::TempDir()) /
          ("sun_moon_archives_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream(dir / "leaf.sun")
        << wrapperSource("slot_lib", ffiTestLibDir() + "/" + kArchiveName);
    std::ofstream(dir / "twin.sun")
        << wrapperSource("slot_twin", ffiTestLibDir() + "/" + kArchiveName);
    std::ofstream(dir / "leaf2.sun")
        << wrapperSource("slot_lib2", ffiTestLibV2Dir() + "/" + kArchiveName);
    std::ofstream(dir / "mid.sun") << R"(
public module slot_mid {
  public function answer() i32 {
    slot_lib.store(37);
    return slot_lib.load() + 5;
  }
}

manifest {
  libraries: [{ path: "leaf.moon" }]
}
)";
    leaf = dir / "leaf.moon";
    mid = dir / "mid.moon";
    twin = dir / "twin.moon";
    leaf2 = dir / "leaf2.moon";
    leafReport = sun::MoonBuilder::build((dir / "leaf.sun").string(), leaf);
    midReport = sun::MoonBuilder::build((dir / "mid.sun").string(), mid);
    sun::MoonBuilder::build((dir / "twin.sun").string(), twin);
    sun::MoonBuilder::build((dir / "leaf2.sun").string(), leaf2);
  }

  // Runs at process exit, after every test in this process is done with it
  ~BundleChain() {
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }
};

const BundleChain& chain() {
  static BundleChain built;
  return built;
}

// Names of the archives a bundle carries, in bundle order.
std::vector<std::string> carriedArchives(const fs::path& bundle) {
  auto reader = sun::MoonReader::open(bundle);
  if (!reader) return {};
  std::vector<std::string> names;
  for (const auto& entry : reader->getNativeArchives()) {
    names.push_back(entry.name);
  }
  return names;
}

bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The `$hash$_` prefix of a bundle's symbols
std::string symbolPrefix(const fs::path& bundle) {
  auto reader = sun::MoonReader::open(bundle);
  if (!reader) return {};
  auto modules = reader->listModules();
  if (modules.empty()) return {};
  const auto* metadata = reader->getMetadata(modules[0]);
  return metadata ? sun::getSymbolPrefix(*metadata) + "_" : "";
}

// The names in the symbol index of the first archive a bundle carries, as C
// code spells them
std::vector<std::string> carriedArchiveSymbols(const fs::path& bundle) {
  auto reader = sun::MoonReader::open(bundle);
  if (!reader || reader->getNativeArchives().empty()) return {};
  std::vector<char> bytes;
  if (!reader->readNativeArchive(reader->getNativeArchives()[0], bytes)) {
    return {};
  }
  return sun::listArchiveIndex(
      llvm::MemoryBufferRef(llvm::StringRef(bytes.data(), bytes.size()), ""));
}

// Build and run a program as a native executable; the exit code, or -1
// when the host cannot link (recorded in `skipReason`).
int runCompiled(Driver& driver, const fs::path& binary,
                std::string& skipReason) {
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.archives = driver.getNativeArchivePaths();
  if (!sun::compileToExecutable(driver.getModule(), binary.string(), errorMsg,
                                /*keepObjectFile=*/false, linkOpts)) {
    skipReason = "host link failed: " + errorMsg;
    return -1;
  }
  // Through a variable: macOS's WEXITSTATUS takes its argument's address
  int rc = std::system(binary.string().c_str());
  return WEXITSTATUS(rc);
}

// A program using both versions of the archive through their bundles.
// v2's `load` adds 1000, so the result tells whether each bundle reached
// its own copy.
constexpr const char* kTwoVersionsProgram = R"(
    function main() i32 {
      slot_lib.store(5);
      slot_lib2.store(7);
      return slot_lib2.load() - slot_lib.load() - 1000 + 40;
    }
  )";

}  // namespace

TEST(Ffi_MoonArchives, bundle_carries_archives_of_the_bundles_it_inlines) {
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";
  const BundleChain& bundles = chain();

  EXPECT_EQ(carriedArchives(bundles.leaf),
            std::vector<std::string>{kArchiveName});
  EXPECT_TRUE(bundles.leafReport.inheritedArchives.empty());

  // mid names no archive of its own, yet carries leaf's exactly once
  EXPECT_TRUE(bundles.midReport.archiveFiles.empty());
  EXPECT_EQ(bundles.midReport.inheritedArchives,
            std::vector<std::string>{kArchiveName});
  EXPECT_EQ(carriedArchives(bundles.mid),
            std::vector<std::string>{kArchiveName});
}

TEST(Ffi_MoonArchives, consumer_of_a_transitive_bundle_links_the_archive) {
  // The shape from issue #218: the program imports only the bundle built on
  // the C wrapper, so the archive reaches the linker through that bundle or
  // not at all.
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";
  const BundleChain& bundles = chain();

  auto driver = Driver::createForAOT("moon_archives_link");
  driver->setMoonImports({sun::MoonImport(bundles.mid.string())});
  driver->compileString(R"(
    function main() i32 {
      return slot_mid.answer();
    }
  )");

  const auto& archives = driver->getNativeArchivePaths();
  ASSERT_EQ(archives.size(), 1u);
  EXPECT_TRUE(endsWith(archives[0], kArchiveName)) << archives[0];

  std::string skipReason;
  int exitCode = runCompiled(*driver, bundles.dir / "consumer", skipReason);
  if (exitCode < 0) GTEST_SKIP() << skipReason;
  EXPECT_EQ(exitCode, 42);
}

TEST(Ffi_MoonArchives, identical_archives_from_two_bundles_are_extracted_once) {
  // A program using both the wrapper and the bundle built on it meets the
  // same archive in both; the link gets one copy.
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";
  const BundleChain& bundles = chain();

  auto driver = Driver::createForAOT("moon_archives_dedup");
  driver->setMoonImports({sun::MoonImport(bundles.leaf.string()),
                          sun::MoonImport(bundles.mid.string())});
  driver->compileString(R"(
    function main() i32 {
      slot_lib.store(1);
      return slot_mid.answer();
    }
  )");

  const auto& archives = driver->getNativeArchivePaths();
  ASSERT_EQ(archives.size(), 1u);
  EXPECT_TRUE(endsWith(archives[0], kArchiveName)) << archives[0];
}

TEST(Ffi_MoonArchives, jit_resolves_a_transitively_carried_archive) {
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";
  const BundleChain& bundles = chain();

  auto driver = Driver::createForJIT("moon_archives_jit");
  driver->setMoonImports({sun::MoonImport(bundles.mid.string())});
  auto value = driver->executeString(R"(
    function main() i32 {
      return slot_mid.answer();
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Symbol isolation: each bundle binds its own copy of a C library
// ============================================================================

TEST(Ffi_MoonArchives, carried_archive_symbols_carry_the_archive_set_hash) {
  // The archive inside leaf.moon defines only names prefixed with the hash
  // of the bundle's archives (not the bundle's hash), and its own extern
  // declarations refer to those names, tagged as C ABI so importers know not
  // to prefix them again.
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";
  const BundleChain& bundles = chain();

  auto reader = sun::MoonReader::open(bundles.leaf);
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(reader->getNativeArchives().size(), 1u);
  const std::string prefix =
      "$" + reader->getNativeArchives()[0].archiveSetHash + "$_";
  EXPECT_EQ(reader->getNativeArchives()[0].archiveSetHash.size(), 16u);
  EXPECT_NE(prefix, symbolPrefix(bundles.leaf));

  const auto symbols = carriedArchiveSymbols(bundles.leaf);
  ASSERT_FALSE(symbols.empty());
  for (const auto& symbol : symbols) {
    EXPECT_EQ(symbol.rfind(prefix, 0), 0u) << "bare symbol: " << symbol;
  }

  llvm::LLVMContext context;
  auto bitcode = reader->loadModule(reader->listModules()[0], context);
  ASSERT_NE(bitcode, nullptr);
  EXPECT_EQ(bitcode->getFunction("sun_ffi_slot_get"), nullptr);
  llvm::Function* renamed = bitcode->getFunction(prefix + "sun_ffi_slot_get");
  ASSERT_NE(renamed, nullptr);
  EXPECT_TRUE(renamed->isDeclaration());
  EXPECT_TRUE(renamed->hasFnAttribute("sun.cabi"));
}

TEST(Ffi_MoonArchives, independent_bundles_carrying_the_same_bytes_share_one) {
  // leaf and twin each list the same archive file in their own manifests.
  // Both rename it under the archive's hash, so the carried bytes match and
  // a program importing both links a single copy, with shared state.
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";
  const BundleChain& bundles = chain();

  EXPECT_EQ(carriedArchiveSymbols(bundles.leaf),
            carriedArchiveSymbols(bundles.twin));

  auto driver = Driver::createForAOT("moon_archives_twin");
  driver->setMoonImports({sun::MoonImport(bundles.leaf.string()),
                          sun::MoonImport(bundles.twin.string())});
  driver->compileString(R"(
    function main() i32 {
      slot_lib.store(42);
      return slot_twin.load();
    }
  )");
  ASSERT_EQ(driver->getNativeArchivePaths().size(), 1u);

  std::string skipReason;
  int exitCode = runCompiled(*driver, bundles.dir / "twin_bin", skipReason);
  if (exitCode < 0) GTEST_SKIP() << skipReason;
  EXPECT_EQ(exitCode, 42);
}

TEST(Ffi_MoonArchives, two_bundles_with_different_versions_link_both) {
  // A program importing leaf (v1) and leaf2 (v2): both archives reach the
  // link, and each bundle's calls land in its own version.
  if (ffiTestLibDir().empty() || ffiTestLibV2Dir().empty()) {
    GTEST_SKIP() << "testlib dir unknown";
  }
  const BundleChain& bundles = chain();

  auto driver = Driver::createForAOT("moon_archives_two_versions");
  driver->setMoonImports({sun::MoonImport(bundles.leaf.string()),
                          sun::MoonImport(bundles.leaf2.string())});
  driver->compileString(kTwoVersionsProgram);

  const auto& archives = driver->getNativeArchivePaths();
  ASSERT_EQ(archives.size(), 2u);
  EXPECT_TRUE(endsWith(archives[0], kArchiveName)) << archives[0];
  EXPECT_TRUE(endsWith(archives[1], kArchiveName)) << archives[1];
  EXPECT_NE(archives[0], archives[1]);

  std::string skipReason;
  int exitCode =
      runCompiled(*driver, bundles.dir / "two_versions", skipReason);
  if (exitCode < 0) GTEST_SKIP() << skipReason;
  EXPECT_EQ(exitCode, 42);
}

TEST(Ffi_MoonArchives, jit_runs_two_versions_like_the_compiled_program) {
  // The JIT loads the same carried archives the linker would, so the result
  // matches the executable. v2's `sun_ffi_slot_set` calls atexit, which the
  // JIT can only resolve because the driver hands over the compiler's own.
  if (ffiTestLibDir().empty() || ffiTestLibV2Dir().empty()) {
    GTEST_SKIP() << "testlib dir unknown";
  }
  const BundleChain& bundles = chain();

  auto driver = Driver::createForJIT("moon_archives_two_versions_jit");
  driver->setMoonImports({sun::MoonImport(bundles.leaf.string()),
                          sun::MoonImport(bundles.leaf2.string())});
  auto value = driver->executeString(kTwoVersionsProgram);
  EXPECT_EQ(value, 42);
}
