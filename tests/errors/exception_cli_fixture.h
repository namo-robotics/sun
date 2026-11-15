/** Shares process execution and isolated files across exception integration
 * tests. */
#pragma once
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

/** Tests public compiler behavior in fresh processes. */
namespace sun::tests::exceptions {
/** Retains a child's diagnostics independently of its exit status. */
struct ProcessResult {
  int status;
  std::string output;
  std::string error;
};

/** Gives each test its own directory, including spaces to exercise path
 * handling. */
class CompilerFixture : public testing::Test {
 protected:
  std::filesystem::path directory;
  const std::string compiler = SUN_TEST_COMPILER;
  const std::filesystem::path root = SUN_TEST_ROOT;
  /** Creates temporary files under the workspace's tmp directory. */
  void SetUp() override;
  /** Removes files even after a failed assertion. */
  void TearDown() override;
  /** Reads a fixture or generated output without changing the working
   * directory. */
  static std::string read(const std::filesystem::path& path);
  /** Writes a source file in this test's directory and returns its path. */
  std::string write(const std::string& name, const std::string& contents);
  /** Copies a named Sun fixture into this test's directory. */
  std::string fixture(const std::string& name);
  /** Runs argv directly, optionally capturing terminal hyperlinks through a
   * PTY. */
  ProcessResult run(std::vector<std::string> command, int expected = 0,
                    bool terminal = false);
  /** Invokes the built compiler and retains failure diagnostics. */
  ProcessResult sun(std::vector<std::string> arguments, int expected = 0);
  /** Compiles a source to a native executable using the selected options. */
  std::string native(const std::string& source,
                     std::vector<std::string> options = {});
  /** Emits unoptimized IR for layout and ownership assertions. */
  std::string ir(const std::string& source,
                 std::vector<std::string> options = {});
  /** Replaces every occurrence in a small source fixture. */
  static std::string replace(std::string text, const std::string& from,
                             const std::string& to);
  /** Finds every occurrence of a diagnostic or hyperlink prefix. */
  static std::size_t occurrences(const std::string& text,
                                 const std::string& part);
  /** Requires linker scratch files to have been removed after success or
   * failure. */
  void expectRuntimeCleanup();

 private:
  unsigned invocation_ = 0;
};
}  // namespace sun::tests::exceptions
