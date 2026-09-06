// tests/ffi/test_moon_archives.cpp — Native archives carried across bundles
//
// A bundle wrapping a C library carries that library's `.a` (manifest
// `archives:`). A second bundle built on the first inlines its bitcode, calls
// included, so it must carry the archive as well: a program importing only
// the second bundle never sees the first one, yet has to link the C code
// (issue #218). The fixture is libsun_ffi_static_testlib.a, whose symbols are
// absent from the test binary and have no shared-library counterpart, so
// neither the AOT link nor the JIT can satisfy them by accident.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/compiler.h"
#include "driver/execution_utils.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "moon_bundling/moon_import.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kArchiveName = "libsun_ffi_static_testlib.a";

// Directory holding the built test archive, baked in by CMake. Empty when the
// define is absent (e.g. an ad-hoc build).
std::string ffiTestLibDir() {
#ifdef SUN_FFI_TESTLIB_DIR
  return SUN_FFI_TESTLIB_DIR;
#else
  return {};
#endif
}

// Two bundles in a scratch directory: `leaf.moon` wraps the C archive and
// carries it; `mid.moon` is built on leaf and names no archive itself. Built
// once per process (see chain()): the library cache is a singleton that keeps
// every bundle it has opened, so rebuilding under a fresh path per test would
// leave it holding readers for files that no longer exist.
struct BundleChain {
  fs::path dir;
  fs::path leaf;
  fs::path mid;
  sun::MoonBuildReport leafReport;
  sun::MoonBuildReport midReport;

  BundleChain() {
    initTestEnvironment();
    dir = fs::path(::testing::TempDir()) / "sun_moon_archives_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string archive = ffiTestLibDir() + "/" + kArchiveName;
    std::ofstream(dir / "leaf.sun") << R"(
public module slot_lib {
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
  archives: [")" << archive
                                << R"("]
}
)";
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
    leafReport = sun::MoonBuilder::build((dir / "leaf.sun").string(), leaf);
    midReport = sun::MoonBuilder::build((dir / "mid.sun").string(), mid);
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

  std::string binary = (bundles.dir / "consumer").string();
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.archives = archives;
  if (!sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                /*keepObjectFile=*/false, linkOpts)) {
    GTEST_SKIP() << "host link failed: " << errorMsg;
  }
  // Through a variable: macOS's WEXITSTATUS takes its argument's address
  int rc = std::system(binary.c_str());
  EXPECT_EQ(WEXITSTATUS(rc), 42);
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
