/** Tests compiler packaging and native exception lowering through the command
 * line. */
#include <unistd.h>

#include <cstdlib>

#include "exception_cli_fixture.h"

/** Keeps build-system integration separate from runtime trace assertions. */
namespace {
using sun::tests::exceptions::CompilerFixture;

/** Selects optimization only for behavior that depends on backend lowering. */
class Errors_NativeCompilation : public CompilerFixture,
                                 public testing::WithParamInterface<bool> {};

/** Describes backend modes in IDE discovery. */
std::string optimizationName(const testing::TestParamInfo<bool>& info) {
  return info.param ? "Optimized" : "Unoptimized";
}

INSTANTIATE_TEST_SUITE_P(Modes, Errors_NativeCompilation, testing::Bool(),
                         optimizationName);

/** Nested cleanup executes correctly in independently linked native programs.
 */
TEST_P(Errors_NativeCompilation, nested_cleanup) {
  const auto source = fixture("source_exceptions.sun");
  run({native(source, GetParam() ? std::vector<std::string>{}
                                 : std::vector<std::string>{"-O0"})},
      42);
}

/** An imported parser releases a partially built container before its caller
 * catches the exception, including with unoptimized native unwind tables. */
TEST_P(Errors_NativeCompilation, imported_cleanup_resumes_unwinding) {
  const auto source = write("cleanup.sun", R"(
using std;
using std.json;
/** Handles malformed input after parser-owned storage has been cleaned up. */
function main() i32 throws {
  var alloc = make_heap_allocator();
  try {
    var value = parse_json(alloc, "[1, 2,]");
  } catch (error: JsonError) {
    return 0;
  }
  return 1;
}
)");
  std::vector<std::string> options{"--moon",
                                   (root / "build/stdlib.moon").string()};
  if (!GetParam()) {
    options.emplace_back("-O0");
  }
  run({native(source, options)});
}

/** Tests compiler distribution and source transformation in isolated
 * directories. */
class Errors_CompilerIntegration : public CompilerFixture {};

/** Exception syntax remains valid and stable after formatting. */
TEST_F(Errors_CompilerIntegration, formatting_preserves_exceptions) {
  const auto source = fixture("source_exceptions.sun");
  sun({"fmt", source});
  sun({"fmt", "--check", source});
  EXPECT_NE(ir(source).find("@sun_eh_personality"), std::string::npos);
}

/** A compiler executable needs no adjacent runtime archive to link a program.
 */
TEST_F(Errors_CompilerIntegration, relocated_compiler_links_embedded_runtime) {
  const auto relocated = directory / "bin" / "sun";
  std::filesystem::create_directories(relocated.parent_path());
  std::filesystem::copy_file(compiler, relocated);
  const auto source = fixture("source_exceptions.sun");
  const auto binary = (directory / "program").string();
  run({relocated.string(), "--dynamic", "-c", source, "-o", binary});
  run({binary}, 42);
  expectRuntimeCleanup();
}

/** Failed links report the unresolved symbol and release extracted runtime
 * files. */
TEST_F(Errors_CompilerIntegration, failed_link_cleans_runtime_files) {
  const auto result =
      sun({"--dynamic", "-c", "--no-test", fixture("unresolved_native.sun"),
           "-o", (directory / "unresolved").string()},
          1);
  const auto diagnostics = result.output + result.error;
  EXPECT_NE(diagnostics.find("Linker failed"), std::string::npos);
  EXPECT_NE(diagnostics.find("sun_missing_link_fixture"), std::string::npos);
  expectRuntimeCleanup();
}

/** Exception propagation uses native unwind operations instead of C++
 * allocation. */
TEST_F(Errors_CompilerIntegration, native_unwind_instructions) {
  const auto text = ir(fixture("source_exceptions.sun"));
  for (const auto* instruction :
       {"invoke ", "landingpad ", "@sun_eh_personality", "@sun_eh_push",
        "@sun_eh_pop"}) {
    EXPECT_NE(text.find(instruction), std::string::npos) << instruction;
  }
  EXPECT_EQ(text.find("__cxa_"), std::string::npos);
}

/** Nested propagation storage belongs to destructors, not every caller or fresh
 * throw. */
TEST_F(Errors_CompilerIntegration, destructor_owns_nested_storage) {
  const auto text = ir(fixture("source_exceptions.sun"));
  EXPECT_NE(text.find("destructor.exception"), std::string::npos);
  EXPECT_EQ(text.find("cleanup.exception"), std::string::npos);
  EXPECT_EQ(text.find("thrown.exception"), std::string::npos);
}

/** Destructors proven unable to initiate exceptions omit propagation storage.
 */
TEST_F(Errors_CompilerIntegration, infallible_destructor_omits_storage) {
  EXPECT_EQ(ir(fixture("infallible_cleanup.sun")).find("destructor.exception"),
            std::string::npos);
}

/** Selects native or JIT execution for the ordinary stdlib wrapper boundary. */
class Errors_TraceWrapper : public CompilerFixture,
                            public testing::WithParamInterface<bool> {};

/** Names the execution backend. */
std::string wrapperBackendName(const testing::TestParamInfo<bool>& info) {
  return info.param ? "Native" : "Jit";
}

INSTANTIATE_TEST_SUITE_P(Backends, Errors_TraceWrapper, testing::Bool(),
                         wrapperBackendName);

/** The imported wrapper prints exactly one retained trace through its
 * borrowed-array ABI. */
TEST_P(Errors_TraceWrapper, reports_once) {
  const auto source = fixture("trace_wrapper.sun");
  const auto bundle =
      (std::filesystem::path(compiler).parent_path() / "stdlib.moon").string();
  const auto output = GetParam()
                          ? run({native(source, {"--moon", bundle})}, 42).error
                          : sun({"--moon", bundle, source}, 42).error;
  EXPECT_EQ(occurrences(output, "Exception trace\n"), 1u);
  EXPECT_NE(output.find("main()"), std::string::npos);
}

/** Chooses an available cross linker without requiring its emulated runtime. */
std::string crossTarget() {
#if defined(__APPLE__) && defined(__aarch64__)
  return "x86_64-apple-darwin";
#elif defined(__linux__) && defined(__x86_64__)
  if (access("/usr/bin/aarch64-linux-gnu-g++", X_OK) == 0) {
    return "aarch64-linux-gnu";
  }
#endif
  return {};
}

/** Embedded runtime source supports a real cross link from a relocated
 * compiler. */
TEST_F(Errors_CompilerIntegration, relocated_compiler_cross_links) {
  const auto target = crossTarget();
  if (target.empty()) {
    GTEST_SKIP() << "No supported cross linker installed";
  }
  const auto relocated = directory / "sun";
  std::filesystem::copy_file(compiler, relocated);
  run({relocated.string(), "--dynamic", "-c", "--target", target,
       fixture("source_exceptions.sun"), "-o", (directory / "cross").string()});
  expectRuntimeCleanup();
}

/** Cross-link failures still release the runtime source and archive scratch
 * files. */
TEST_F(Errors_CompilerIntegration, failed_cross_link_cleans_runtime_files) {
  const auto target = crossTarget();
  if (target.empty()) {
    GTEST_SKIP() << "No supported cross linker installed";
  }
  const auto result = sun({"--dynamic", "-c", "--no-test", "--target", target,
                           fixture("unresolved_native.sun"), "-o",
                           (directory / "cross-unresolved").string()},
                          1);
  EXPECT_NE((result.output + result.error).find("sun_missing_link_fixture"),
            std::string::npos);
  expectRuntimeCleanup();
}
}  // namespace
