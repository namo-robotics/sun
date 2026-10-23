/** Regression coverage for terminal messages and strict static linking. */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "driver/compiler.h"
#include "support/error.h"
#include "support/terminal.h"
#include "support/terminal_format.h"

/** Keeps process-environment fixtures local to these tests. */
namespace {

/** Restores an environment variable after a test changes it. */
class ScopedEnvironment {
 public:
  /** Saves the existing value and installs or removes the test value. */
  ScopedEnvironment(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name)) old_ = old;
    if (value)
      setenv(name, value, 1);
    else
      unsetenv(name);
  }
  /** Restores the value present before the test. */
  ~ScopedEnvironment() {
    if (old_)
      setenv(name_.c_str(), old_->c_str(), 1);
    else
      unsetenv(name_.c_str());
  }

 private:
  std::string name_;
  std::optional<std::string> old_;
};

/** Simulates a color-capable terminal without requiring a real terminal device. */
class ColoredStringStream : public llvm::raw_string_ostream {
 public:
  /** Captures output in the supplied string. */
  explicit ColoredStringStream(std::string& output)
      : llvm::raw_string_ostream(output) {}
  /** Enables terminal styling for isolated rendering checks. */
  bool has_colors() const override { return true; }
};

/** Keeps test files isolated and removes them even when an assertion fails. */
class TerminalFileFixture : public testing::Test {
 protected:
  std::filesystem::path directory;
  /** Creates a scratch directory under the workspace temporary directory. */
  void SetUp() override {
    directory = std::filesystem::current_path() / "tmp" / "terminal_format";
    std::filesystem::create_directories(directory);
  }
  /** Removes this fixture's scratch files. */
  void TearDown() override { std::filesystem::remove_all(directory); }
};

/** Trees preserve ancestor guides, details, and sibling order without escapes. */
TEST(Tooling_Cli_Terminal, tree_layout_supports_nested_details) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  const sun::support::TerminalTreeNode root{
      "main.sun", {},
      {{"reader.moon", {}, {{"shapes", {"artifact: old", "ABI: same"}, {}}}},
       {"shapes", {"artifact: new"}, {}}}};
  EXPECT_EQ(sun::support::terminalTree(root, stream, "warning", "  "),
            "  main.sun\n"
            "  ├── reader.moon\n"
            "  │   └── shapes\n"
            "  │       artifact: old\n"
            "  │       ABI: same\n"
            "  └── shapes\n"
            "      artifact: new");
  EXPECT_EQ(sun::support::terminalTree({"alone", {}, {}}, stream), "alone");
}

/** Tree colors match severity prefixes while labels retain their own style. */
TEST(Tooling_Cli_Terminal, tree_colors_only_connectors) {
  ScopedEnvironment term("TERM", "xterm");
  ScopedEnvironment noColor("NO_COLOR", nullptr);
  std::string output;
  ColoredStringStream stream(output);
  const sun::support::TerminalTreeNode root{"root", {}, {{"leaf", {}, {}}}};
  EXPECT_EQ(sun::support::terminalTree(root, stream),
            "root\n\033[1;33m└── \033[0mleaf");
  EXPECT_EQ(sun::support::terminalTree(root, stream, "error"),
            "root\n\033[1;31m└── \033[0mleaf");
  {
    ScopedEnvironment disabled("NO_COLOR", "1");
    EXPECT_EQ(sun::support::terminalTree(root, stream), "root\n└── leaf");
  }
  ScopedEnvironment dumb("TERM", "dumb");
  EXPECT_EQ(sun::support::terminalTree(root, stream), "root\n└── leaf");
}

/** File links encode special bytes and retain plain fallbacks and VS Code paths. */
TEST_F(TerminalFileFixture, links_preserve_paths_and_terminal_preferences) {
  ScopedEnvironment term("TERM", "xterm");
  ScopedEnvironment noColor("NO_COLOR", nullptr);
  ScopedEnvironment program("TERM_PROGRAM", "other");
  const auto file = directory / "source #%.sun";
  std::ofstream(file) << "test";
  std::string output;
  ColoredStringStream stream(output);
  llvm::raw_string_ostream plain(output);
  const auto link = sun::support::terminalFileLabel(file, stream);
  EXPECT_NE(link.find("source%20%23%25.sun"), std::string::npos);
  EXPECT_NE(link.find("\033[4:1;58;5;3msource #%.sun\033[24;59m"),
            std::string::npos);
  EXPECT_NE(link.find("\033]8;;file://"), std::string::npos);
  EXPECT_TRUE(link.ends_with("\033]8;;\033\\"));
  EXPECT_EQ(sun::support::terminalFileLabel(file, plain), "source #%.sun");
  EXPECT_EQ(sun::support::terminalFileLabel(directory / "missing.sun", stream),
            "missing.sun");
  EXPECT_EQ(sun::support::terminalFileLabel(directory, stream), "terminal_format");
  {
    ScopedEnvironment vscode("TERM_PROGRAM", "vscode");
    EXPECT_EQ(sun::support::terminalFileLabel(file, stream),
              "\033[4:1;58;5;3m" + file.string() + "\033[24;59m");
  }
  {
    ScopedEnvironment disabled("NO_COLOR", "1");
    EXPECT_EQ(sun::support::terminalFileLabel(file, stream), "source #%.sun");
  }
  ScopedEnvironment dumb("TERM", "dumb");
  EXPECT_EQ(sun::support::terminalFileLabel(file, stream), "source #%.sun");
}

/** Checks that captured diagnostics remain plain text with severity labels. */
TEST(Tooling_Cli_Terminal, redirected_messages_have_plain_prefixes) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  EXPECT_EQ(sun::support::messagePrefix("warning", stream), "[sun][warning]: ");
  ScopedEnvironment noColor("NO_COLOR", "1");
  sun::support::SunError error(sun::support::SunError::Kind::Type, "bad type");
  EXPECT_EQ(std::string(error.what()), "[sun][error]: Type Error: bad type");
}

/** Checks all lines in a multiline message receive a prefix. */
TEST(Tooling_Cli_Terminal, multiline_warnings_are_prefixed) {
  ScopedEnvironment noColor("NO_COLOR", "1");
  testing::internal::CaptureStderr();
  sun::support::logMessage("warning", "first\nsecond\n");
  llvm::errs().flush();
  EXPECT_EQ(testing::internal::GetCapturedStderr(),
            "[sun][warning]: first\n[sun][warning]: second\n");
}

/** Default logging hides build progress while retaining compiler warnings. */
TEST(Tooling_Cli_Terminal, default_level_is_warn) {
  ScopedEnvironment level("SUN_LOG_LEVEL", nullptr);
  ScopedEnvironment noColor("NO_COLOR", "1");
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  sun::support::logMessage("info", "progress");
  sun::support::logMessage("warning", "warning");
  sun::support::logMessage("error", "failure");
  llvm::outs().flush();
  llvm::errs().flush();
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
  EXPECT_EQ(testing::internal::GetCapturedStderr(),
            "[sun][warning]: warning\n[sun][error]: failure\n");
}

/** Explicit info logging restores progress messages on stdout. */
TEST(Tooling_Cli_Terminal, info_level_restores_progress) {
  ScopedEnvironment level("SUN_LOG_LEVEL", "info");
  ScopedEnvironment noColor("NO_COLOR", "1");
  testing::internal::CaptureStdout();
  sun::support::logMessage("info", "progress");
  llvm::outs().flush();
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "[sun][info]: progress\n");
}

/** Error-only logging suppresses warnings without suppressing errors. */
TEST(Tooling_Cli_Terminal, error_level_filters_warnings) {
  ScopedEnvironment level("SUN_LOG_LEVEL", "error");
  EXPECT_FALSE(sun::support::messageEnabled("info"));
  EXPECT_FALSE(sun::support::messageEnabled("warning"));
  EXPECT_TRUE(sun::support::messageEnabled("error"));
}

/** Unknown settings fall back to the default warning threshold. */
TEST(Tooling_Cli_Terminal, unknown_level_uses_warn) {
  ScopedEnvironment level("SUN_LOG_LEVEL", "invalid");
  EXPECT_FALSE(sun::support::messageEnabled("info"));
  EXPECT_TRUE(sun::support::messageEnabled("warning"));
}

/** Checks that missing musl cannot silently choose a host or cross compiler. */
TEST(Tooling_Cli_Terminal, missing_musl_requires_explicit_link_choice) {
  ScopedEnvironment path("PATH", "");
  ScopedEnvironment cc("SUN_CC", nullptr);
  EXPECT_EQ(sun::driver::linkerCommandFor("x86_64-linux-gnu", true), "");
  EXPECT_EQ(sun::driver::linkerCommandFor("aarch64-linux-gnu", true), "");
  EXPECT_EQ(sun::driver::linkerCommandFor("", false), "cc");
  sun::driver::LinkOptions options;
  options.targetTriple = "x86_64-linux-gnu";
  options.staticLink = true;
  std::string error;
  EXPECT_FALSE(
      sun::driver::linkExecutable("unused.o", "unused", error, options));
  EXPECT_NE(error.find("x86_64-linux-musl-gcc"), std::string::npos);
  EXPECT_NE(error.find("--dynamic"), std::string::npos);
}

/** Checks that an explicit toolchain override remains supported. */
TEST(Tooling_Cli_Terminal, explicit_driver_is_respected) {
  ScopedEnvironment path("PATH", "");
  ScopedEnvironment cc("SUN_CC", "custom-driver");
  EXPECT_EQ(sun::driver::linkerCommandFor("x86_64-linux-gnu", true),
            "custom-driver");
}

/** Checks external diagnostics and decoded failure status use Sun formatting.
 */
TEST(Tooling_Cli_Terminal, linker_output_preserves_severity_and_exit_status) {
  ScopedEnvironment noColor("NO_COLOR", "1");
  ScopedEnvironment cc(
      "SUN_CC",
      "sh -c 'echo warning: test-warning; echo error: test-error; exit 7' --");
  testing::internal::CaptureStderr();
  std::string error;
  EXPECT_FALSE(sun::driver::linkExecutable("unused.o", "unused", error));
  llvm::errs().flush();
  EXPECT_EQ(testing::internal::GetCapturedStderr(),
            "[sun][warning]: warning: test-warning\n"
            "[sun][error]: error: test-error\n");
  EXPECT_EQ(error, "Linker failed with exit code: 7");
}

}  // namespace
