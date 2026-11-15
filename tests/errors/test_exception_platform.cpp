/** Runs allocation-denial probes in fresh native or JIT processes. */
#include <cstdlib>
#include <regex>
#include <stdexcept>

#include "exception_cli_fixture.h"

/** Keeps allocator interception out of the Google Test process itself. */
namespace {
using sun::tests::exceptions::CompilerFixture;

/** Builds runtime-only probes at both optimization levels without LLVM headers.
 */
class Errors_NativePlatform : public CompilerFixture,
                              public testing::WithParamInterface<bool> {
 protected:
  /** Uses configured tools by default and supports a runnable alternate target.
   */
  std::string probe() {
    const char* requestedTarget = std::getenv("SUN_EXCEPTION_PROBE_TARGET");
    const char* requestedCompiler = std::getenv("SUN_EXCEPTION_PROBE_COMPILER");
    if (GetParam() && !requestedTarget && !requestedCompiler) {
      return SUN_TEST_NATIVE_PROBE;
    }
    const std::string clang =
        requestedCompiler ? requestedCompiler : SUN_TEST_CLANG;
    std::vector<std::string> prefix{clang, "--driver-mode=g++"};
    if (requestedTarget) {
      prefix.push_back(std::string("--target=") + requestedTarget);
    }
    auto tripleCommand = prefix;
    tripleCommand.insert(tripleCommand.end(), {"-S", "-emit-llvm", "-x", "c",
                                               "/dev/null", "-o", "-"});
    const auto module = run(std::move(tripleCommand)).output;
    std::smatch match;
    if (!std::regex_search(module, match,
                           std::regex("target triple = \"([^\"]+)\""))) {
      throw std::runtime_error("Clang did not emit a target triple");
    }
    const auto llvm =
        write("fixture.ll",
              replace(read(root / "tests/errors/fixtures/exceptions.ll.in"),
                      "@SUN_EXCEPTION_TARGET@", match[1]));
    const auto object = (directory / "fixture.o").string();
    auto compile = prefix;
    const std::string optimization = GetParam() ? "-O2" : "-O0";
    compile.insert(compile.end(),
                   {"-Werror", "-x", "ir", "-c", "-fPIC", optimization,
                    "-mllvm", "-global-isel=0", llvm, "-o", object});
    run(std::move(compile));
    const auto binary = (directory / "allocation-probe").string();
    auto link = prefix;
    link.insert(link.end(),
                {"-std=c++20", optimization, "-fno-builtin", "-funwind-tables",
                 "-pthread", "-I" + (root / "include").string()});
    for (const auto* source :
         {"tests/errors/exception_allocation_probe.cpp",
          "tests/errors/exception_allocation_support.cpp",
          "tests/errors/exception_capture_probe.cpp",
          "tests/errors/allocation_audit.cpp", "src/runtime/exceptions.cpp",
          "src/runtime/exception_trace.cpp", "src/runtime/exception_report.cpp",
          "src/runtime/exception_sources.cpp",
          "src/runtime/exception_personality.cpp", "src/runtime/panic.cpp",
          "src/driver/jit/exception_storage.cpp"}) {
      link.push_back((root / source).string());
    }
    link.insert(link.end(), {object, "-ldl", "-o", binary});
    run(std::move(link));
    return binary;
  }
};

/** Makes optimizer variants explicit in CTest and IDE discovery. */
std::string nativeModeName(const testing::TestParamInfo<bool>& info) {
  return info.param ? "Optimized" : "Unoptimized";
}

INSTANTIATE_TEST_SUITE_P(Modes, Errors_NativePlatform, testing::Bool(),
                         nativeModeName);

/** The first stack walk cannot allocate, including when visiting new native
 * frames. */
TEST_P(Errors_NativePlatform, cold_capture_without_allocations) {
  run({probe(), "--capture"});
}

/** The first throw and subsequent reporting operate while allocation is denied.
 */
TEST_P(Errors_NativePlatform, transport_and_reporting_without_allocations) {
  run({probe()});
}

/** Independent threads retain separate propagation records without allocating
 * during transport. */
TEST_P(Errors_NativePlatform, concurrent_transport_without_allocations) {
  run({probe(), "--threads"});
}

/** Launches JIT probes after the test runner's own allocations have finished.
 */
class Errors_JitAllocation : public CompilerFixture {};

/** Prepared JIT unwind tables support the first throw without heap allocation.
 */
TEST_F(Errors_JitAllocation, first_throw) {
  run({SUN_TEST_JIT_PROBE, "--jit"});
}

/** Loading and unloading code does not leave stale unwind metadata. */
TEST_F(Errors_JitAllocation, module_reload) {
  run({SUN_TEST_JIT_PROBE, "--jit-reload"});
}

/** Concurrent JIT transport retains independent thread state. */
TEST_F(Errors_JitAllocation, concurrent_transport) {
  run({SUN_TEST_JIT_PROBE, "--jit-threads"});
}
}  // namespace
