/** Checks layout selection, bundle compatibility, and public trace-depth
 * settings. */
#include "abi/exception_layout.h"
#include "exception_cli_fixture.h"

/** Keeps depth scenarios independent of output-format tests. */
namespace {
using sun::tests::exceptions::CompilerFixture;

/** Selects a storage boundary and an execution backend. */
struct DepthMode {
  unsigned depth;
  bool native;
};

/** Describes parameter values without exposing raw object bytes in CTest. */
void PrintTo(const DepthMode& mode, std::ostream* output) {
  *output << (mode.native ? "Native" : "Jit") << " depth " << mode.depth;
}

/** Runs each supported boundary in its own compiler and program processes. */
class Errors_TraceDepth : public CompilerFixture,
                          public testing::WithParamInterface<DepthMode> {};

/** Gives each boundary and backend a stable discovery name. */
std::string depthModeName(const testing::TestParamInfo<DepthMode>& info) {
  return std::string(info.param.native ? "Native" : "Jit") +
         std::to_string(info.param.depth);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, Errors_TraceDepth,
    testing::Values(DepthMode{0, false}, DepthMode{1, false},
                    DepthMode{16, false}, DepthMode{256, false},
                    DepthMode{0, true}, DepthMode{1, true}, DepthMode{16, true},
                    DepthMode{256, true}),
    depthModeName);

/** Capture honors the selected depth in both runtime loading paths. */
TEST_P(Errors_TraceDepth, capture_status) {
  const auto source = fixture("trace_capture.sun");
  const auto setting =
      "--exception-trace-depth=" + std::to_string(GetParam().depth);
  const auto output = GetParam().native
                          ? run({native(source, {setting})}, 42).error
                          : sun({setting, source}, 42).error;
  EXPECT_NE(output.find("Exception trace\n"), std::string::npos);
  EXPECT_EQ(output.find("Trace disabled") != std::string::npos,
            GetParam().depth == 0);
  EXPECT_EQ(output.find("Trace unavailable"), std::string::npos);
  EXPECT_EQ(output.find("Trace partial"), std::string::npos);
  if (GetParam().depth > 1) {
    EXPECT_EQ(output.find("Trace truncated"), std::string::npos);
  }
}

/** Inspects one emitted layout per boundary rather than repeating it for each
 * backend. */
class Errors_TraceLayout : public CompilerFixture,
                           public testing::WithParamInterface<unsigned> {};

/** Uses the configured capacity as the discovery name. */
std::string layoutName(const testing::TestParamInfo<unsigned>& info) {
  return "Depth" + std::to_string(info.param);
}

INSTANTIATE_TEST_SUITE_P(Boundaries, Errors_TraceLayout,
                         testing::Values(0u, 1u, 16u, 256u), layoutName);

/** Generated storage and the compatibility contract agree on depth. */
TEST_P(Errors_TraceLayout, generated_storage) {
  const auto depth = GetParam();
  const auto text = ir(fixture("trace_capture.sun"),
                       {"--exception-trace-depth=" + std::to_string(depth)});
  const auto size = sun::abi::exceptions::valueSizeForDepth(depth);
  EXPECT_NE(text.find("alloca [" + std::to_string(size) + " x i8]"),
            std::string::npos);
  EXPECT_NE(text.find("!\"sun.exceptions.trace_depth\", i32 " +
                      std::to_string(depth)),
            std::string::npos);
}

/** Creates a depth-one bundle whose compatibility can be tested independently.
 */
class Errors_TraceBundles : public CompilerFixture {
 protected:
  std::string library, bundle, source;

  /** Builds only the shared prerequisites for one bundle scenario. */
  void SetUp() override {
    CompilerFixture::SetUp();
    library = fixture("trace_library.sun");
    source = fixture("trace_import.sun");
    bundle = (directory / "failures.moon").string();
    sun({"--exception-trace-depth=1", "--emit-moon", library, "-o", bundle});
  }
};

/** Rejects incompatible layouts before code can use the wrong storage size. */
TEST_F(Errors_TraceBundles, rejects_mismatched_depth) {
  const auto output =
      sun({"--exception-trace-depth=16", "--moon", bundle, source}, 1).error;
  EXPECT_NE(output.find("exception trace depth mismatch"), std::string::npos);
}

/** Matching imported code retains its original library frame. */
TEST_F(Errors_TraceBundles, imported_trace_in_jit) {
  const auto output =
      sun({"--exception-trace-depth=1", "--moon", bundle, source}, 42).error;
  EXPECT_NE(output.find("fail()"), std::string::npos);
  EXPECT_NE(output.find("Trace truncated"), std::string::npos);
}

/** Native linking preserves source metadata from the matching bundle. */
TEST_F(Errors_TraceBundles, imported_trace_in_native_code) {
  const auto binary =
      native(source, {"--exception-trace-depth=1", "--moon", bundle});
  EXPECT_NE(run({binary}, 42).error.find("fail()"), std::string::npos);
}

/** Changing depth invalidates a previously built Moon product. */
TEST_F(Errors_TraceBundles, depth_invalidates_bundle_cache) {
  const auto same =
      sun({"--exception-trace-depth=1", "--emit-moon", library, "-o", bundle});
  EXPECT_NE((same.output + same.error).find("Up to date"), std::string::npos);
  const auto changed =
      sun({"--exception-trace-depth=0", "--emit-moon", library, "-o", bundle});
  EXPECT_EQ((changed.output + changed.error).find("Up to date"),
            std::string::npos);
}

/** Exercises each configuration rule without sharing state between tests. */
class Errors_TraceConfiguration : public CompilerFixture {
 protected:
  /** Writes a minimal project with one capture entrypoint. */
  std::string configure(const std::string& depth) {
    fixture("trace_capture.sun");
    return write("sun-config.json",
                 "{\"root\":true,\"exception_trace_depth\":" + depth +
                     ",\"entrypoints\":[{\"path\":\"trace_capture.sun\","
                     "\"type\":\"binary\"}]}");
  }
};

/** The project value applies when compiling its source directly. */
TEST_F(Errors_TraceConfiguration, source_uses_project_depth) {
  configure("0");
  EXPECT_NE(sun({(directory / "trace_capture.sun").string()}, 42)
                .error.find("Trace disabled"),
            std::string::npos);
}

/** Building via the configuration entrypoint uses the same project value. */
TEST_F(Errors_TraceConfiguration, configuration_entrypoint_uses_depth) {
  EXPECT_NE(sun({configure("0")}, 42).error.find("Trace disabled"),
            std::string::npos);
}

/** An explicit command-line choice overrides the project setting. */
TEST_F(Errors_TraceConfiguration, command_line_overrides_project) {
  configure("0");
  EXPECT_NE(sun({"--exception-trace-depth=1",
                 (directory / "trace_capture.sun").string()},
                42)
                .error.find("Trace truncated"),
            std::string::npos);
}

/** A changed project depth rebuilds an existing native executable. */
TEST_F(Errors_TraceConfiguration, depth_invalidates_native_cache) {
  configure("0");
  const auto source = (directory / "trace_capture.sun").string();
  const auto binary = native(source);
  EXPECT_NE(run({binary}, 42).error.find("Trace disabled"), std::string::npos);
  configure("1");
  const auto changed = sun({"--dynamic", "-c", source, "-o", binary});
  EXPECT_EQ((changed.output + changed.error).find("Up to date"),
            std::string::npos);
  EXPECT_NE(run({binary}, 42).error.find("main()"), std::string::npos);
}

/** Gives invalid CLI values separate discoverable results. */
class Errors_InvalidTraceDepth
    : public CompilerFixture,
      public testing::WithParamInterface<const char*> {};

INSTANTIATE_TEST_SUITE_P(Values, Errors_InvalidTraceDepth,
                         testing::Values("-1", "257", "1.5", "no",
                                         "999999999999999999"));

/** Malformed or out-of-range CLI settings are rejected. */
TEST_P(Errors_InvalidTraceDepth, command_line) {
  const auto output = sun({std::string("--exception-trace-depth=") + GetParam(),
                           fixture("trace_capture.sun")},
                          1)
                          .error;
  EXPECT_NE(output.find("between 0 and 256"), std::string::npos);
}

/** Gives invalid JSON types and ranges separate discoverable results. */
class Errors_InvalidProjectTraceDepth
    : public Errors_TraceConfiguration,
      public testing::WithParamInterface<const char*> {};

INSTANTIATE_TEST_SUITE_P(Values, Errors_InvalidProjectTraceDepth,
                         testing::Values("-1", "257", "1.5", "\"1\""));

/** Project settings reject both wrong types and invalid ranges. */
TEST_P(Errors_InvalidProjectTraceDepth, project_configuration) {
  configure(GetParam());
  EXPECT_NE(sun({(directory / "trace_capture.sun").string()}, 1)
                .error.find("exception_trace_depth"),
            std::string::npos);
}

/** Current-stack reports share the configured depth without requiring an
 * exception. */
TEST_F(Errors_TraceConfiguration, current_stack_disabled) {
  const auto source = write("current.sun",
                            "/** Reports here. */\nfunction main() void { "
                            "_report_stack_trace(); }\n");
  EXPECT_NE(sun({"--exception-trace-depth=0", source})
                .error.find("Stack trace\nTrace disabled\n"),
            std::string::npos);
}

/** A nondefault JIT runtime also supplies the current-stack reporter. */
TEST_F(Errors_TraceConfiguration, current_stack_nondefault_runtime) {
  const auto source = write("current.sun",
                            "/** Reports here. */\nfunction main() void { "
                            "_report_stack_trace(); }\n");
  const auto output = sun({"--exception-trace-depth=1", source}).error;
  EXPECT_NE(output.find("Stack trace\n"), std::string::npos);
  EXPECT_NE(output.find("Trace truncated"), std::string::npos);
}
}  // namespace
