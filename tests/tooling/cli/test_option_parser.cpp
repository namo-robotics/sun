// Command-line parsing for the sun executable: which flags each command
// accepts, what they set, and which combinations are turned away. The parser
// prints nothing, so the message and exit code of a rejected command line are
// read straight from the EarlyExit it returns.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "cli/option_parser.h"
#include "cli/usage.h"

using sun::cli::BuildRunOptions;
using sun::cli::EarlyExit;
using sun::cli::FmtOptions;
using sun::cli::TestOptions;

/** Keeps test fixtures and helpers local to this source file. */
namespace {

using Args = std::vector<std::string>;

const char* const kProgram = "prog";

/** Holds parsed build/run options and an optional early-exit result. */
struct ParsedBuildRun {
  BuildRunOptions options;
  std::optional<EarlyExit> early;
};

/** Holds parsed test-command options and an optional early-exit result. */
struct ParsedTest {
  TestOptions options;
  std::optional<EarlyExit> early;
};

/** Holds parsed formatter options and an optional early-exit result. */
struct ParsedFmt {
  FmtOptions options;
  std::optional<EarlyExit> early;
};

/** Parses fixture arguments using the build/run command parser. */
ParsedBuildRun parseBuildRun(const Args& args) {
  ParsedBuildRun parsed;
  parsed.early =
      sun::cli::parseBuildRunArguments(kProgram, args, parsed.options);
  return parsed;
}

/** Parses fixture arguments using the test-command parser. */
ParsedTest parseTest(const Args& args) {
  ParsedTest parsed;
  parsed.early = sun::cli::parseTestArguments(args, parsed.options);
  return parsed;
}

/** Parses fixture arguments using the formatter-command parser. */
ParsedFmt parseFmt(const Args& args) {
  ParsedFmt parsed;
  parsed.early = sun::cli::parseFmtArguments(args, parsed.options);
  return parsed;
}

/**
 * The command line was turned away on stderr with exactly this message.
 */
void expectRejected(const std::optional<EarlyExit>& early,
                    const std::string& message, int exitCode = 1) {
  ASSERT_TRUE(early.has_value());
  EXPECT_EQ(early->exitCode, exitCode);
  EXPECT_EQ(early->stream, EarlyExit::Stream::Err);
  EXPECT_EQ(early->text, message);
}

/** Reports whether text begins with the expected prefix. */
bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

}  // namespace

// ============================================================================
// Default command: what the flags set
// ============================================================================

TEST(Tooling_Cli_OptionParser, plain_script_runs_with_the_jit) {
  auto parsed = parseBuildRun({"app.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_EQ(parsed.options.inputFiles, Args({"app.sun"}));
  EXPECT_FALSE(parsed.options.compileMode);
  EXPECT_FALSE(parsed.options.emitMoon);
  EXPECT_FALSE(parsed.options.configInput);
  EXPECT_TRUE(parsed.options.shared.optimize);
  EXPECT_FALSE(parsed.options.shared.debugInfo);
  EXPECT_TRUE(parsed.options.programArgs.empty());
}

TEST(Tooling_Cli_OptionParser, compile_flags_set_their_fields) {
  auto parsed = parseBuildRun({"-c", "-o", "out", "app.sun", "-g", "-O0",
                               "--debug", "--emit-ir", "--no-test",
                               "--dump-proto-sun", "--skip-if-unchanged"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_TRUE(parsed.options.compileMode);
  EXPECT_FALSE(parsed.options.emitObjOnly);
  EXPECT_EQ(parsed.options.outputFile, "out");
  EXPECT_TRUE(parsed.options.shared.debugInfo);
  EXPECT_FALSE(parsed.options.shared.optimize);
  EXPECT_TRUE(parsed.options.shared.debugMode);
  EXPECT_TRUE(parsed.options.shared.emitIR);
  EXPECT_TRUE(parsed.options.noTest);
  EXPECT_TRUE(parsed.options.dumpProtoSun);
  EXPECT_TRUE(parsed.options.skipIfUnchanged);
}

TEST(Tooling_Cli_OptionParser, emit_obj_implies_compile_mode) {
  auto parsed = parseBuildRun({"--emit-obj", "app.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_TRUE(parsed.options.compileMode);
  EXPECT_TRUE(parsed.options.emitObjOnly);
}

TEST(Tooling_Cli_OptionParser, native_library_flags_take_both_spellings) {
  auto parsed = parseBuildRun({"-lm", "-l", "ssl", "-Lvendor", "-L", "/opt/lib",
                               "--sysroot", "/sysroot", "app.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_EQ(parsed.options.linkOptions.libraries, Args({"m", "ssl"}));
  EXPECT_EQ(parsed.options.linkOptions.searchPaths,
            Args({"vendor", "/opt/lib"}));
  EXPECT_EQ(parsed.options.linkOptions.sysroot, "/sysroot");
}

// -l<name> matches on its prefix, so any other flag starting with -l is read
// as a library name.
TEST(Tooling_Cli_OptionParser, dash_l_prefix_swallows_lookalike_flags) {
  auto parsed = parseBuildRun({"-link", "app.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_EQ(parsed.options.linkOptions.libraries, Args({"ink"}));
}

TEST(Tooling_Cli_OptionParser, shared_flags_are_collected_in_order) {
  auto parsed = parseBuildRun(
      {"--path-var", "A=1", "--path-var", "A=2", "--path-var",
       "EMPTY=", "--lib-path", "one", "--lib-path", "two", "--moon",
       "lib.moon:std=std_v1", "--gh-token", "tok", "app.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  const auto& shared = parsed.options.shared;
  ASSERT_EQ(shared.pathVariables.size(), 3u);
  EXPECT_EQ(shared.pathVariables[0],
            std::make_pair(std::string("A"), std::string("1")));
  EXPECT_EQ(shared.pathVariables[1],
            std::make_pair(std::string("A"), std::string("2")));
  EXPECT_EQ(shared.pathVariables[2],
            std::make_pair(std::string("EMPTY"), std::string("")));
  EXPECT_EQ(shared.libPaths, Args({"one", "two"}));
  ASSERT_EQ(shared.moonImports.size(), 1u);
  EXPECT_EQ(shared.moonImports[0].path, "lib.moon");
  EXPECT_EQ(parsed.options.githubToken, "tok");
}

TEST(Tooling_Cli_OptionParser, arguments_after_double_dash_go_to_the_program) {
  auto parsed = parseBuildRun({"app.sun", "--", "a", "--help", "b"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_EQ(parsed.options.inputFiles, Args({"app.sun"}));
  EXPECT_EQ(parsed.options.programArgs, Args({"a", "--help", "b"}));

  auto bare = parseBuildRun({"app.sun", "--"});
  ASSERT_FALSE(bare.early.has_value());
  EXPECT_TRUE(bare.options.programArgs.empty());
}

// Without -- every extra name is another source file, not a program argument.
TEST(Tooling_Cli_OptionParser, extra_positionals_are_more_input_files) {
  auto parsed = parseBuildRun({"a.sun", "b.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_EQ(parsed.options.inputFiles, Args({"a.sun", "b.sun"}));
}

// A flag that takes a value takes the next argument whatever it looks like.
TEST(Tooling_Cli_OptionParser, value_flags_take_the_next_argument_as_is) {
  auto parsed = parseBuildRun({"-c", "-o", "--emit-ir", "app.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_EQ(parsed.options.outputFile, "--emit-ir");
  EXPECT_FALSE(parsed.options.shared.emitIR);
}

TEST(Tooling_Cli_OptionParser, config_input_is_a_lone_sun_config_json) {
  auto lone = parseBuildRun({"-c", "proj/sun-config.json"});
  ASSERT_FALSE(lone.early.has_value());
  EXPECT_TRUE(lone.options.configInput);

  auto withSibling = parseBuildRun({"-c", "proj/sun-config.json", "b.sun"});
  ASSERT_FALSE(withSibling.early.has_value());
  EXPECT_FALSE(withSibling.options.configInput);
}

TEST(Tooling_Cli_OptionParser, linking_is_static_unless_dynamic_or_macos) {
  auto byDefault =
      parseBuildRun({"-c", "--target", "x86_64-linux-gnu", "app.sun"});
  ASSERT_FALSE(byDefault.early.has_value());
  EXPECT_TRUE(byDefault.options.linkOptions.staticLink);

  auto dynamic = parseBuildRun(
      {"-c", "--dynamic", "--target", "x86_64-linux-gnu", "app.sun"});
  ASSERT_FALSE(dynamic.early.has_value());
  EXPECT_FALSE(dynamic.options.linkOptions.staticLink);

  auto macos =
      parseBuildRun({"-c", "--target", "arm64-apple-darwin", "app.sun"});
  ASSERT_FALSE(macos.early.has_value());
  EXPECT_FALSE(macos.options.linkOptions.staticLink);
}

// ============================================================================
// Default command: early exits
// ============================================================================

TEST(Tooling_Cli_OptionParser, help_prints_usage_on_stderr_and_succeeds) {
  for (const char* flag : {"-h", "--help"}) {
    auto parsed = parseBuildRun({"app.sun", flag});
    ASSERT_TRUE(parsed.early.has_value());
    EXPECT_EQ(parsed.early->exitCode, 0);
    EXPECT_EQ(parsed.early->stream, EarlyExit::Stream::Err);
    EXPECT_EQ(parsed.early->text, sun::cli::renderUsage(kProgram));
  }
  EXPECT_TRUE(startsWith(sun::cli::renderUsage(kProgram),
                         "Usage: prog [options] <script.sun>"));
}

TEST(Tooling_Cli_OptionParser, version_prints_on_stdout) {
  auto parsed = parseBuildRun({"--version"});
  ASSERT_TRUE(parsed.early.has_value());
  EXPECT_EQ(parsed.early->exitCode, 0);
  EXPECT_EQ(parsed.early->stream, EarlyExit::Stream::Out);
  EXPECT_TRUE(startsWith(parsed.early->text, "sun "));
  EXPECT_EQ(parsed.early->text.back(), '\n');
}

// Arguments are read left to right, so the first problem wins.
TEST(Tooling_Cli_OptionParser, unknown_option_prints_usage_and_fails) {
  auto parsed = parseBuildRun({"--bogus", "--help"});
  expectRejected(parsed.early,
                 "Unknown option: --bogus\n" + sun::cli::renderUsage(kProgram));
}

// A flag missing its value is reported as unknown; so is -S, which the help
// text lists but nothing implements.
TEST(Tooling_Cli_OptionParser, value_flag_without_a_value_is_unknown) {
  for (const char* flag : {"-o", "--target", "--sysroot", "--lib-path", "-l",
                           "-L", "--moon", "--gh-token", "--path-var", "-S"}) {
    auto parsed = parseBuildRun({"app.sun", flag});
    expectRejected(parsed.early, std::string("Unknown option: ") + flag + "\n" +
                                     sun::cli::renderUsage(kProgram));
  }
}

TEST(Tooling_Cli_OptionParser, malformed_moon_and_path_var_are_rejected) {
  expectRejected(parseBuildRun({"--moon", "lib.moon:std=", "app.sun"}).early,
                 "Invalid --moon format: lib.moon:std=\n"
                 "Expected: path.moon or path.moon:module=alias\n");
  expectRejected(parseBuildRun({"--path-var", "novalue", "app.sun"}).early,
                 "Invalid --path-var format: novalue\n"
                 "Expected: NAME=<dir>\n");
  expectRejected(parseBuildRun({"--path-var", "=dir", "app.sun"}).early,
                 "Invalid --path-var format: =dir\n"
                 "Expected: NAME=<dir>\n");
}

TEST(Tooling_Cli_OptionParser, missing_input_file_is_rejected) {
  const std::string noInput =
      "Error: No input file specified.\n" + sun::cli::renderUsage(kProgram);
  expectRejected(parseBuildRun({}).early, noInput);
  expectRejected(parseBuildRun({"-c"}).early, noInput);
  // Parsing stops at --, so the name after it is a program argument
  expectRejected(parseBuildRun({"--", "app.sun"}).early, noInput);
  expectRejected(parseBuildRun({"--emit-moon"}).early,
                 "Error: --emit-moon requires an entrypoint file\n");
}

// ============================================================================
// Default command: rejected combinations
// ============================================================================

TEST(Tooling_Cli_OptionParser, target_needs_a_build_mode) {
  const std::string message =
      "Error: --target requires --emit-obj, -c or --emit-moon (JIT execution "
      "is host-only)\n";
  expectRejected(
      parseBuildRun({"--target", "aarch64-linux-gnu", "a.sun"}).early, message);
  for (const char* mode : {"-c", "--emit-obj", "--emit-moon"}) {
    auto parsed =
        parseBuildRun({mode, "--target", "aarch64-linux-gnu", "a.sun"});
    EXPECT_FALSE(parsed.early.has_value()) << mode;
  }
}

TEST(Tooling_Cli_OptionParser, static_and_dynamic_exclude_each_other) {
  const std::string message =
      "Error: --static and --dynamic are mutually exclusive\n";
  expectRejected(parseBuildRun({"-c", "--static", "--dynamic", "a.sun"}).early,
                 message);
  // Checked before "--static needs -c", so this is the message without -c too
  expectRejected(parseBuildRun({"--static", "--dynamic", "a.sun"}).early,
                 message);
}

TEST(Tooling_Cli_OptionParser, no_test_only_applies_when_linking) {
  const std::string message =
      "Error: --no-test only applies when linking an executable; use it with "
      "-c\n";
  expectRejected(parseBuildRun({"--no-test", "a.sun"}).early, message);
  expectRejected(parseBuildRun({"--emit-obj", "--no-test", "a.sun"}).early,
                 message);
  expectRejected(parseBuildRun({"--emit-moon", "--no-test", "a.sun"}).early,
                 message);
  EXPECT_FALSE(parseBuildRun({"-c", "--no-test", "a.sun"}).early.has_value());
}

TEST(Tooling_Cli_OptionParser, static_only_applies_when_linking) {
  const std::string message =
      "Error: --static only applies when linking; use it with -c\n";
  expectRejected(parseBuildRun({"--static", "a.sun"}).early, message);
  expectRejected(parseBuildRun({"--emit-obj", "--static", "a.sun"}).early,
                 message);
  // --dynamic has no such rule
  EXPECT_FALSE(parseBuildRun({"--dynamic", "a.sun"}).early.has_value());
}

TEST(Tooling_Cli_OptionParser, static_is_not_supported_for_macos) {
  expectRejected(parseBuildRun({"-c", "--static", "--target",
                                "arm64-apple-darwin", "a.sun"})
                     .early,
                 "Error: --static is not supported for macOS targets\n");
}

TEST(Tooling_Cli_OptionParser, skip_if_unchanged_needs_a_build_mode) {
  expectRejected(parseBuildRun({"--skip-if-unchanged", "a.sun"}).early,
                 "Error: --skip-if-unchanged is about built artifacts; use it "
                 "with -c or --emit-moon\n");
  EXPECT_FALSE(parseBuildRun({"--emit-moon", "--skip-if-unchanged", "a.sun"})
                   .early.has_value());
}

TEST(Tooling_Cli_OptionParser, config_input_rejects_moon_obj_and_output_name) {
  const std::string wrongMode =
      "Error: a sun-config.json input works with -c or plain run; libraries "
      "in its entrypoints already build their .moon bundles under -c\n";
  expectRejected(parseBuildRun({"--emit-moon", "sun-config.json"}).early,
                 wrongMode);
  expectRejected(parseBuildRun({"--emit-obj", "sun-config.json"}).early,
                 wrongMode);
  expectRejected(parseBuildRun({"-c", "-o", "out", "sun-config.json"}).early,
                 "Error: -o does not combine with a sun-config.json input; "
                 "the config's output_name fields name the artifacts\n");
}

// ============================================================================
// sun test
// ============================================================================

TEST(Tooling_Cli_OptionParser, test_forwards_runner_flags_in_order) {
  auto parsed = parseTest({"app.sun", "--test-filter", "geo.*",
                           "--test-sequential", "--", "z", "--bogus"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_EQ(parsed.options.inputFile, "app.sun");
  EXPECT_EQ(
      parsed.options.forwardedArgs,
      Args({"--test-filter", "geo.*", "--test-sequential", "z", "--bogus"}));
}

TEST(Tooling_Cli_OptionParser, test_accepts_the_shared_flags) {
  auto parsed =
      parseTest({"--debug", "-O0", "-g", "--emit-ir", "--lib-path", "build",
                 "--moon", "lib.moon", "--path-var", "A=b", "app.sun"});
  ASSERT_FALSE(parsed.early.has_value());
  const auto& shared = parsed.options.shared;
  EXPECT_TRUE(shared.debugMode);
  EXPECT_FALSE(shared.optimize);
  EXPECT_TRUE(shared.emitIR);
  EXPECT_EQ(shared.libPaths, Args({"build"}));
  EXPECT_EQ(shared.moonImports.size(), 1u);
  EXPECT_EQ(shared.pathVariables.size(), 1u);
  EXPECT_TRUE(parsed.options.forwardedArgs.empty());
}

TEST(Tooling_Cli_OptionParser, test_rejects_flags_of_the_default_command) {
  for (const Args& args : {Args{"-lm"}, Args{"--gh-token", "tok"},
                           Args{"--target", "t"}, Args{"-h"}, Args{"--help"},
                           Args{"-c"}, Args{"-o", "out"}, Args{"--version"}}) {
    Args full = args;
    full.push_back("app.sun");
    expectRejected(parseTest(full).early,
                   "Unknown option for 'sun test': " + args[0] + "\n" +
                       sun::cli::kTestUsageFull);
  }
}

TEST(Tooling_Cli_OptionParser, test_value_flag_without_a_value_is_unknown) {
  for (const char* flag :
       {"--test-filter", "--lib-path", "--moon", "--path-var"}) {
    expectRejected(parseTest({"app.sun", flag}).early,
                   std::string("Unknown option for 'sun test': ") + flag +
                       "\n" + sun::cli::kTestUsageFull);
  }
}

TEST(Tooling_Cli_OptionParser, test_takes_exactly_one_entrypoint) {
  expectRejected(parseTest({}).early, sun::cli::kTestUsageBrief);
  expectRejected(parseTest({"--debug"}).early, sun::cli::kTestUsageBrief);
  expectRejected(parseTest({"a.sun", "b.sun"}).early,
                 "'sun test' takes one entrypoint file\n");
  // A malformed flag is reported ahead of the missing file
  expectRejected(parseTest({"--path-var", "novalue"}).early,
                 "Invalid --path-var format: novalue\n"
                 "Expected: NAME=<dir>\n");
}

// ============================================================================
// sun fmt
// ============================================================================

TEST(Tooling_Cli_OptionParser, fmt_collects_inputs_and_check_mode) {
  auto parsed = parseFmt({"a.sun", "--check", "src"});
  ASSERT_FALSE(parsed.early.has_value());
  EXPECT_TRUE(parsed.options.checkMode);
  EXPECT_EQ(parsed.options.inputs, Args({"a.sun", "src"}));

  EXPECT_FALSE(parseFmt({"a.sun"}).options.checkMode);
}

TEST(Tooling_Cli_OptionParser, fmt_help_succeeds_and_mistakes_exit_with_two) {
  auto help = parseFmt({"--help"});
  ASSERT_TRUE(help.early.has_value());
  EXPECT_EQ(help.early->exitCode, 0);
  EXPECT_EQ(help.early->stream, EarlyExit::Stream::Err);
  EXPECT_EQ(help.early->text, sun::cli::kFmtUsage);

  expectRejected(parseFmt({"--bogus", "a.sun"}).early,
                 "Unknown fmt option: --bogus\n", /*exitCode=*/2);
  expectRejected(parseFmt({"--"}).early, "Unknown fmt option: --\n",
                 /*exitCode=*/2);
  expectRejected(parseFmt({}).early, sun::cli::kFmtUsage, /*exitCode=*/2);
  expectRejected(parseFmt({"--check"}).early, sun::cli::kFmtUsage,
                 /*exitCode=*/2);
}
