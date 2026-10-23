/** Regression coverage for terminal messages and strict static linking. */
#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "driver/compiler.h"
#include "support/error.h"
#include "support/terminal.h"

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
