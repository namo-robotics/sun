/** Exercises readable reports through the compiler's public native and JIT
 * paths. */
#include <llvm/TargetParser/Host.h>

#include <array>
#include <regex>
#include <sstream>

#include "exception_cli_fixture.h"

/** Keeps reporting fixtures independent of compiler implementation details. */
namespace {
using sun::tests::exceptions::CompilerFixture;

/** Selects execution and metadata modes that affect reporting. */
struct ReportingMode {
  const char* name;
  bool native;
  bool optimize;
  bool debug;
  bool explicitTarget = false;
};

/** Describes parameter values without exposing raw object bytes in CTest. */
void PrintTo(const ReportingMode& mode, std::ostream* output) {
  *output << mode.name;
}

/** Prepares one source fixture and the selected backend. */
class Errors_TraceReporting
    : public CompilerFixture,
      public testing::WithParamInterface<ReportingMode> {
 protected:
  /** Chooses compiler flags without duplicating test bodies. */
  std::vector<std::string> options() const {
    std::vector<std::string> result;
    if (GetParam().explicitTarget) {
      result.insert(result.end(),
                    {"--target", llvm::sys::getDefaultTargetTriple()});
    }
    if (!GetParam().optimize) {
      result.emplace_back("-O0");
    }
    if (GetParam().debug) {
      result.emplace_back("-g");
    }
    return result;
  }

  /** Produces a command whose exit behavior belongs to the source program. */
  std::vector<std::string> command(const std::string& source) {
    if (GetParam().native) {
      auto binary = native(source, options());
      // Embedded source indexes must work without external debug files.
      std::filesystem::remove(binary + ".o");
      std::filesystem::remove_all(binary + ".dSYM");
      return {binary};
    }
    auto args = options();
    args.insert(args.begin(), compiler);
    args.push_back(source);
    return args;
  }

  /** Uses a source name containing spaces to check source-path encoding. */
  std::string program(bool current = false) {
    auto text = read(root / "tests/errors/fixtures/pretty_exception.sun");
    if (current) {
      text =
          replace(replace(text, "throw Failure();", "_report_stack_trace();"),
                  " throws", "");
    }
    return write("source with spaces.sun", text);
  }

  /** Checks outermost-first frames, two-space nesting, and the originating
   * source lines. */
  void expectTree(const std::string& output, const std::string& source,
                  bool exception) {
    EXPECT_NE(output.find(exception ? "Unhandled exception: Failure\n"
                                    : "Stack trace\n"),
              std::string::npos);
    const std::array<std::string, 4> labels{
        "  main()", "  └─ foo()", "    └─ bar()", "      └─ throw Failure()"};
    const std::array<int, 4> lines{17, 12, 7, 7};
    const auto path =
        GetParam().debug ? source : "ENTRYPOINT/source with spaces.sun";
    std::vector<std::string> frames;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);) {
      if (line.starts_with("  ")) {
        frames.push_back(line);
      }
    }
    ASSERT_EQ(frames.size(), exception ? 4u : 3u) << output;
    for (std::size_t i = 0; i < frames.size(); ++i) {
      EXPECT_TRUE(frames[i].starts_with(labels[i] + "  " + path + ":" +
                                        std::to_string(lines[i]) + ":"))
          << output;
    }
    EXPECT_EQ(output.find("Trace complete"), std::string::npos);
  }
};

/** Names backend variants explicitly in CTest and IDE discovery. */
std::string reportingModeName(
    const testing::TestParamInfo<ReportingMode>& info) {
  return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(
    Backends, Errors_TraceReporting,
    testing::Values(ReportingMode{"JitOptimized", false, true, false},
                    ReportingMode{"JitUnoptimized", false, false, false},
                    ReportingMode{"NativeOptimized", true, true, false},
                    ReportingMode{"NativeUnoptimized", true, false, false},
                    ReportingMode{"JitDebug", false, true, true},
                    ReportingMode{"NativeDebug", true, true, true},
                    ReportingMode{"NativeTarget", true, true, false, true},
                    ReportingMode{"NativeTargetDebug", true, true, true, true}),
    reportingModeName);

/** An uncaught exception reports its original source call chain. */
TEST_P(Errors_TraceReporting, exception_tree) {
  const auto source = program();
  expectTree(run(command(source), 1).error, source, true);
}

/** Ordinary functions can report their current stack without throws or a catch.
 */
TEST_P(Errors_TraceReporting, current_stack) {
  const auto source = program(true);
  expectTree(run(command(source)).error, source, false);
}

/** Terminal links carry source locations while the visible tree stays
 * uncluttered. */
TEST_P(Errors_TraceReporting, hover_locations) {
  const auto source = program();
  const auto output = run(command(source), 1, true).output;
  const auto target = GetParam().debug
                          ? "vscode://file" + replace(source, " ", "%20")
                          : "sun-source:ENTRYPOINT/source%20with%20spaces.sun";
  for (int line : {7, 12, 17}) {
    EXPECT_NE(
        output.find("\033]8;;" + target + ":" + std::to_string(line) + ":"),
        std::string::npos)
        << output;
  }
  EXPECT_EQ(occurrences(output, "\033]8;;" + target), 4u);
  const auto plain =
      std::regex_replace(output, std::regex("\033\\]8;;[^\033]*\033\\\\"), "");
  EXPECT_NE(
      plain.find(
          "  main()\n  └─ foo()\n    └─ bar()\n      └─ throw Failure()\n"),
      std::string::npos)
      << plain;
  EXPECT_EQ(plain.find("ENTRYPOINT/"), std::string::npos);
  EXPECT_EQ(plain.find(source + ":"), std::string::npos);
}

/** Applies metadata-path checks independently of report rendering. */
class Errors_TraceMetadata : public CompilerFixture,
                             public testing::WithParamInterface<bool> {};

/** Names whether absolute debug paths are expected. */
std::string metadataModeName(const testing::TestParamInfo<bool>& info) {
  return info.param ? "Debug" : "Release";
}

INSTANTIATE_TEST_SUITE_P(Modes, Errors_TraceMetadata, testing::Bool(),
                         metadataModeName);

/** Only debug builds store the original absolute source directory. */
TEST_P(Errors_TraceMetadata, source_path_policy) {
  const auto source = fixture("pretty_exception.sun");
  const auto text = ir(source, GetParam() ? std::vector<std::string>{"-g"}
                                          : std::vector<std::string>{});
  EXPECT_EQ(text.find(directory.string()) != std::string::npos, GetParam());
  EXPECT_NE(text.find("pretty_exception.sun"), std::string::npos);
}

/** Gives focused caught-report checks an isolated source file. */
class Errors_ExceptionReporting : public CompilerFixture {};

/** Rethrowing preserves exactly the original report rather than capturing the
 * catch. */
TEST_F(Errors_ExceptionReporting, rethrow_preserves_report) {
  const auto text = replace(
      read(root / "tests/errors/fixtures/pretty_exception.sun"),
      "function main() void throws {\n  foo();\n}",
      "function main() void {\n  try { foo(); } catch (error: Failure) {\n"
      "    _report_exception();\n"
      "    try { throw error; } catch { _report_exception(); }\n  }\n}");
  const auto output = sun({write("rethrow.sun", text)}).error;
  const std::string heading = "Exception: Failure\n";
  ASSERT_EQ(occurrences(output, heading), 2u) << output;
  const auto second =
      output.find(heading, output.find(heading) + heading.size());
  EXPECT_EQ(output.substr(0, second), output.substr(second));
  EXPECT_NE(output.find("throw Failure()"), std::string::npos);
}

/** Imported exception names never expose bundle hashes. */
TEST_F(Errors_ExceptionReporting, imported_type_name) {
  const auto source =
      write("imported.sun",
            "using std;\n/** Throws an imported type. */\n"
            "function main() void throws { throw Error(1, \"fixture\"); }\n");
  const auto output =
      sun({"--moon",
           (std::filesystem::path(compiler).parent_path() / "stdlib.moon")
               .string(),
           source},
          1)
          .error;
  EXPECT_NE(output.find("Unhandled exception: Error\n"), std::string::npos);
  EXPECT_EQ(output.find('$'), std::string::npos);
}
}  // namespace
